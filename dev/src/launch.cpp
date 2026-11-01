/* LLcl - launch.c : natives 解压(自研zipx)、命令行拼接、@argfile 超长回退、
 * CreateProcessW 启动 + 分段时间统计 */
#include "llcl.h"

#define CMDLINE_MAX (30000)

void lresult_init(lresult* r) { memset(r, 0, sizeof(*r)); }

/* ---------------- natives extraction (own zip reader, zero dependency) ---------------- */
/* 解压整个 natives jar 到目录；返回解出文件数（失败返回 0） */
static int extract_zip(const wchar_t* zip, const wchar_t* dest) {
    int n = 0;
    if (!dir_exists(dest) && !ensure_dir_rec(dest)) return 0;
    if (!zip_extract_prefix(zip, NULL, dest, &n)) return 0;
    return n > 0;
}

/* 统计目录里的 .dll 数量（用于校验 natives 是否完整，而不是只看目录存在）。 */
static int count_dlls_in_dir(const wchar_t* dir) {
    if (!dir_exists(dir)) return 0;
    wchar_t* pat = path_join(dir, L"*.dll");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void remove_tree(const wchar_t* p) {
    wchar_t* pat = path_join(p, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    free(pat);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t* full = path_join(p, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(full);
                RemoveDirectoryW(full);
            } else {
                SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full);
            }
            free(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(p);
}

/* natives 指纹：native jar 集合 + 文件大小 + 解压子目录。版本更新或
 * 中断解压都会导致指纹变化，从而触发一次完整重解压。 */
static void natives_stamp(const vinfo* v, const wchar_t* suffix, wchar_t* out, size_t cap) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < v->nlibs; i++) {
        if (!v->libs[i].is_native) continue;
        if (wcsstr(v->libs[i].name, L"arm64")) continue;
        for (const wchar_t* p = v->libs[i].rel; p && *p; p++) {
            h ^= (uint64_t)(unsigned short)*p;
            h *= 1099511628211ULL;
        }
        h ^= v->libs[i].size;
        h *= 1099511628211ULL;
    }
    if (suffix) {
        for (const wchar_t* p = suffix; *p; p++) { h ^= (uint64_t)(unsigned short)*p; h *= 1099511628211ULL; }
    }
    _snwprintf_s(out, cap, _TRUNCATE, L"%016llx", (unsigned long long)h);
}

/* ---------------- substitution ---------------- */
typedef struct {
    const wchar_t* player;
    const wchar_t* uuid;
    const wchar_t* token;
    const wchar_t* user_type;
    const wchar_t* version_name;
    const wchar_t* version_type;
    const wchar_t* game_dir;
    const wchar_t* assets_root;
    const wchar_t* assets_index;
    const wchar_t* natives;
    const wchar_t* classpath;
    const wchar_t* lib_dir;
    const wchar_t* launcher_name;
    const wchar_t* launcher_version;
    const wchar_t* auth_session;
    const wchar_t* xuid;
    const wchar_t* server_ip;
} subst_ctx;

static const wchar_t* subst_lookup(const subst_ctx* c, const wchar_t* key, int* found) {
    *found = 1;
    if (weq(key, L"auth_player_name")) return c->player;
    if (weq(key, L"auth_uuid")) return c->uuid;
    if (weq(key, L"auth_access_token")) return c->token;
    if (weq(key, L"auth_session")) return c->auth_session;
    if (weq(key, L"user_type")) return c->user_type;
    if (weq(key, L"version_name")) return c->version_name;
    if (weq(key, L"version_type")) return c->version_type;
    if (weq(key, L"game_directory")) return c->game_dir;
    if (weq(key, L"assets_root")) return c->assets_root;
    if (weq(key, L"assets_index_name")) return c->assets_index;
    if (weq(key, L"natives_directory")) return c->natives;
    if (weq(key, L"classpath")) return c->classpath;
    if (weq(key, L"classpath_separator")) return L";";
    if (weq(key, L"library_directory")) return c->lib_dir;
    if (weq(key, L"launcher_name")) return c->launcher_name;
    if (weq(key, L"launcher_version")) return c->launcher_version;
    if (weq(key, L"quickPlayPath")) return L"";
    /* 离线/无正版会话时的占位值：Minecraft 官方 clientId + xuid=0，
       保证 --clientId/--xuid 参数配对完整、游戏不报错（不影响进入游戏） */
    if (weq(key, L"clientid")) return L"00000000402b5328";
    if (weq(key, L"auth_xuid")) return (c->xuid && c->xuid[0]) ? c->xuid : L"0";
    if (weq(key, L"server_ip")) return c->server_ip;
    *found = 0;
    return NULL;
}

static wchar_t* subst_str(const wchar_t* in, const subst_ctx* c) {
    wsb out;
    wsb_init(&out);
    const wchar_t* p = in;
    while (*p) {
        if (p[0] == L'$' && p[1] == L'{') {
            const wchar_t* e = wcschr(p + 2, L'}');
            if (e) {
                size_t klen = (size_t)(e - (p + 2));
                wchar_t* key = (wchar_t*)xalloc((klen + 1) * sizeof(wchar_t));
                memcpy(key, p + 2, klen * sizeof(wchar_t));
                key[klen] = 0;
                int found = 0;
                const wchar_t* val = subst_lookup(c, key, &found);
                if (found) {
                    if (val) wsb_add(&out, val);
                    p = e + 1;
                    free(key);
                    continue;
                }
                free(key);
            }
        }
        wsb_addc(&out, *p);
        p++;
    }
    return wsb_take(&out);
}

/* ---------------- command line ---------------- */
static wchar_t* quote_arg(const wchar_t* arg) {
    int need = 0;
    for (const wchar_t* p = arg; *p; p++)
        if (*p == L' ' || *p == L'\t') { need = 1; break; }
    if (!need) return wdup(arg);
    wsb b;
    wsb_init(&b);
    wsb_addc(&b, L'"');
    for (const wchar_t* p = arg; *p; p++) {
        if (*p == L'"') wsb_addc(&b, L'\\');
        wsb_addc(&b, *p);
    }
    wsb_addc(&b, L'"');
    return wsb_take(&b);
}

static void args_push(wsa* a, const wchar_t* s) { wsa_add(a, s); }

/* 运行内存优化：注入 G1GC 低停顿参数（Java 8+；早于 8 用默认，避免老 JVM 不识别参数）。
 * 按堆大小选 region；不使用 AlwaysPreTouch（老机器启动慢、强制占满堆）。
 * 参考 Aikar flags，针对客户端（小堆、低延迟）保守化。 */
static void jvm_mem_flags(int heap, int java_major, wsa* args) {
    if (java_major < 8) return;
    const wchar_t* region;
    if (heap <= 512)      region = L"1M";
    else if (heap < 1536) region = L"4M";
    else if (heap < 4096) region = L"8M";
    else                  region = L"16M";
    args_push(args, L"-XX:+UseG1GC");
    args_push(args, L"-XX:+UnlockExperimentalVMOptions");
    args_push(args, L"-XX:+ParallelRefProcEnabled");
    args_push(args, L"-XX:+DisableExplicitGC");
    args_push(args, L"-XX:MaxGCPauseMillis=50");
    args_push(args, L"-XX:G1NewSizePercent=20");
    args_push(args, L"-XX:G1ReservePercent=20");
    {
        wchar_t t[64];
        _snwprintf_s(t, 64, _TRUNCATE, L"-XX:G1HeapRegionSize=%s", region);
        args_push(args, t);
    }
    args_push(args, L"-XX:G1HeapWastePercent=5");
    args_push(args, L"-XX:G1MixedGCCountTarget=4");
    args_push(args, L"-XX:SurvivorRatio=32");
    args_push(args, L"-XX:+PerfDisableSharedMem");
    args_push(args, L"-XX:MaxTenuringThreshold=1");
}

/* join args into one command line with quoting */
static wchar_t* join_cmdline(const wsa* a, size_t* len_out) {
    wsb b;
    wsb_init(&b);
    for (size_t i = 0; i < a->n; i++) {
        if (i) wsb_addc(&b, L' ');
        wchar_t* q = quote_arg(a->v[i]);
        wsb_add(&b, q);
        free(q);
    }
    *len_out = b.n;
    return wsb_take(&b);
}

/* write args (except first=java.exe) to an @argfile (UTF-8, one arg per line).
 * JDK argfile rules: whitespace separates; quotes group; backslash only escapes
 * '"', '\'' and '\' — single backslashes in Windows paths are kept literal. */
static wchar_t* write_argfile(const wchar_t* dir, const wsa* a) {
    wchar_t* path = path_join(dir, L"LLcl-args.txt");
    FILE* f = NULL;
    _wfopen_s(&f, path, L"wb");
    if (!f) { free(path); return NULL; }
    for (size_t i = 1; i < a->n; i++) {
        int need = 0;
        for (const wchar_t* p = a->v[i]; *p; p++)
            if (*p == L' ' || *p == L'\t' || *p == L'"') { need = 1; break; }
        char* u8 = w2u(a->v[i]);
        if (!u8) continue;
        if (!need) {
            fputs(u8, f);
            fputs("\r\n", f);
        } else {
            fputc('"', f);
            for (const char* p = u8; *p; p++) {
                if (*p == '"') fputc('\\', f);
                fputc(*p, f);
            }
            fputs("\"\r\n", f);
        }
        free(u8);
    }
    fclose(f);
    return path;
}

/* 多人服务器直连：下一条启动命令带上 --server/--port。
 * 由 UI 在启动前设置，launch_game 取走后立即清空。 */
static wchar_t g_pending_server[256] = L"";

void launch_set_server(const wchar_t* addr) {
    if (addr && addr[0]) wcsncpy_s(g_pending_server, 256, addr, _TRUNCATE);
    else g_pending_server[0] = 0;
}

void do_launch_with_server(const wchar_t* server_ip) {
    launch_set_server(server_ip);
}

static void add_server_args(wsa* args, const wchar_t* addr) {
    if (!addr || !addr[0]) return;
    wchar_t host[256];
    int port = 0;
    wcsncpy_s(host, 256, addr, _TRUNCATE);
    wchar_t* colon = wcsrchr(host, L':');
    if (colon && colon != host) {
        port = _wtoi(colon + 1);
        *colon = 0;
        if (port <= 0 || port > 65535) port = 0;
    }
    if (host[0] == L'[') {
        size_t n = wcslen(host);
        if (n > 1 && host[n - 1] == L']') host[n - 1] = 0;
        memmove(host, host + 1, (wcslen(host)) * sizeof(wchar_t));
    }
    if (!host[0]) return;
    args_push(args, L"--server");
    args_push(args, host);
    if (port > 0 && port != 25565) {
        wchar_t ps[16];
        _snwprintf_s(ps, 16, _TRUNCATE, L"%d", port);
        args_push(args, L"--port");
        args_push(args, ps);
    }
}

/* Internal launch function that includes server IP */
static int launch_game_internal(const vinfo* v, const wchar_t* gamedir, const wchar_t* java_exe,
                 int mem_mb, const wchar_t* player, const wchar_t* uuid_in,
                 const wchar_t* access_token, const wchar_t* xuid, int isolate,
                 int mem_opt, lresult* r, const wchar_t* server_ip);

/* Public launch function with optional server IP */
int launch_game(const vinfo* v, const wchar_t* gamedir, const wchar_t* java_exe,
                int mem_mb, const wchar_t* player, const wchar_t* uuid_in,
                const wchar_t* access_token, const wchar_t* xuid, int isolate,
                int mem_opt, lresult* r) {
    wchar_t server[256] = L"";
    wcsncpy_s(server, 256, g_pending_server, _TRUNCATE);
    g_pending_server[0] = 0;
    return launch_game_internal(v, gamedir, java_exe, mem_mb, player, uuid_in,
                                access_token, xuid, isolate, mem_opt, r,
                                server[0] ? server : NULL);
}

/* Internal launch function with server IP support */
static int launch_game_internal(const vinfo* v, const wchar_t* gamedir, const wchar_t* java_exe,
                 int mem_mb, const wchar_t* player, const wchar_t* uuid_in,
                 const wchar_t* access_token, const wchar_t* xuid, int isolate,
                 int mem_opt, lresult* r, const wchar_t* server_ip) {
    lresult_init(r);
    uint64_t t0 = now_us();
    if (!file_exists(java_exe)) {
        _snwprintf_s(r->err, 640, _TRUNCATE, L"找不到 Java: %s", java_exe);
        return 0;
    }
    /* 0. version isolation: 隔离时游戏数据目录 = <gamedir>\versions\<id>\instance */
    wchar_t* game_dir_eff = NULL;
    if (isolate) {
        wchar_t* inst_parent = path_join3(gamedir, L"versions", v->id);
        game_dir_eff = path_join(inst_parent, L"instance");
        free(inst_parent);
        if (!dir_exists(game_dir_eff) && !ensure_dir_rec(game_dir_eff)) {
            _snwprintf_s(r->err, 640, _TRUNCATE, L"无法创建版本隔离目录: %s", game_dir_eff);
            free(game_dir_eff);
            return 0;
        }
    } else {
        game_dir_eff = wdup(gamedir);
    }
    wchar_t uuid[64], token[4096];
    int online = access_token && access_token[0];
    if (uuid_in && uuid_in[0]) wcsncpy_s(uuid, 64, uuid_in, _TRUNCATE);
    else uuid_offline(player, uuid, 64);
    if (online) {
        wcsncpy_s(token, 4096, access_token, _TRUNCATE);
    } else {
        /* 离线模式：accessToken 使用去掉横线的离线 UUID（原版离线启动的惯例） */
        wchar_t tk[64];
        int j = 0;
        for (int i = 0; uuid[i] && j < 63; i++)
            if (uuid[i] != L'-') tk[j++] = uuid[i];
        tk[j] = 0;
        wcscpy_s(token, 4096, tk);
    }

    /* 1. natives dir
     * 26.x 的 jvm 参数模板形如 -Djava.library.path=${natives_directory}/java，
     * 即 dll 要平铺到 natives 下的子目录（java\）。老版本用 natives 根。
     * 这里从模板提取子目录后缀，解压目标与之对齐。 */
    const wchar_t* lp_suffix = NULL;
    for (size_t i = 0; i < v->jvm.n; i++) {
        const wchar_t* t = v->jvm.v[i];
        if (wstartswith(t, L"-Djava.library.path=${natives_directory}")) {
            const wchar_t* s = t + wcslen(L"-Djava.library.path=${natives_directory}");
            while (*s == L'/' || *s == L'\\') s++;
            if (*s) lp_suffix = s;
            break;
        }
    }
    wchar_t* vdir = path_join3(gamedir, L"versions", v->id);
    wchar_t* natives = path_join(vdir, L"natives");
    wchar_t* natives_target = NULL;
    if (lp_suffix) natives_target = path_join(natives, lp_suffix);
    wchar_t* extract_dir = natives_target ? natives_target : natives;
    int native_count = 0;
    for (size_t i = 0; i < v->nlibs; i++)
        if (v->libs[i].is_native) native_count++;

    wchar_t stamp[32] = L"";
    natives_stamp(v, lp_suffix, stamp, 32);
    wchar_t* marker = path_join(natives, L".llcl_natives");
    int natives_ready = 0;
    if (native_count > 0 && file_exists(marker)) {
        FILE* mf = _wfopen(marker, L"rb");
        char mb[128] = "";
        if (mf) { fread(mb, 1, sizeof(mb) - 1, mf); fclose(mf); }
        char want[64] = "";
        wchar_t wstamp[64];
        _snwprintf_s(wstamp, 64, _TRUNCATE, L"%s", stamp);
        char* stamp8 = w2u(wstamp);
        if (stamp8) { strncpy_s(want, sizeof(want), stamp8, _TRUNCATE); free(stamp8); }
        int want_count = -1;
        char* bar = strchr(mb, '|');
        if (bar) want_count = atoi(bar + 1);
        int actual = count_dlls_in_dir(extract_dir);
        if (strncmp(mb, want, strlen(want)) == 0 && want_count > 0 && actual == want_count)
            natives_ready = 1;
    }
    if (native_count > 0 && !natives_ready) {
        /* 旧目录里可能残留旧版本/半次解压的 dll；整体清空后完整重解压，
           否则游戏可能加载到错误架构或旧版本的 dll。 */
        if (dir_exists(natives)) remove_tree(natives);
        if (!ensure_dir_rec(extract_dir)) {
            _snwprintf_s(r->err, 640, _TRUNCATE, L"无法创建 natives 目录: %s", extract_dir);
            free(marker); free(natives_target); free(natives); free(vdir); free(game_dir_eff);
            return 0;
        }
        int extracted = 0, failed = 0;
        for (size_t i = 0; i < v->nlibs; i++) {
            if (!v->libs[i].is_native) continue;
            if (!v->libs[i].present) {
                _snwprintf_s(r->err, 640, _TRUNCATE,
                             L"缺少原生库: %s\n请在游戏目录补全 libraries 后重试", v->libs[i].name);
                free(marker); free(natives_target); free(natives); free(vdir); free(game_dir_eff);
                return 0;
            }
            /* 只解本机架构（x64）的 natives：跳过 arm64 同名 jar，避免平铺覆盖 */
            if (wcsstr(v->libs[i].name, L"arm64") != NULL) continue;
            if (wcsstr(v->libs[i].fullpath, L"arm64") != NULL) continue;
            int dn = 0;
            if (zip_extract_dlls(v->libs[i].fullpath, extract_dir, &dn) && dn > 0) extracted++;
            else failed = 1;
        }
        int actual = count_dlls_in_dir(extract_dir);
        if (actual == 0 || failed) {
            _snwprintf_s(r->err, 640, _TRUNCATE,
                         L"原生库解压失败（%d 个 jar，已解出 %d 个 dll），目录: %s",
                         native_count, actual, extract_dir);
            free(marker); free(natives_target); free(natives); free(vdir); free(game_dir_eff);
            return 0;
        }
        FILE* mf = _wfopen(marker, L"wb");
        if (mf) {
            char* s8 = w2u(stamp);
            if (s8) { fprintf(mf, "%s|%d", s8, actual); free(s8); }
            fclose(mf);
        }
    }
    free(marker);
    r->us_natives = now_us() - t0;

    /* 2. classpath — libraries 依赖 + 游戏本体 jar (versions/<id>/<id>.jar) */
    wsb cp;
    wsb_init(&cp);
    int missing = 0;
    /* Forge/NeoForge 的 jvm 用 -p 模块路径：其 classpath 不能含原版 client jar
     * （会被 BootstrapLauncher 转成自动模块，与运行时 patch 的 minecraft 模块冲突），
     * 只放 libraries 供其扫描。 */
    int uses_module_path = 0;
    for (size_t i = 0; i < v->jvm.n; i++)
        if (weq(v->jvm.v[i], L"-p") || weq(v->jvm.v[i], L"--module-path")) { uses_module_path = 1; break; }
    /* 本体 jar：继承链根版本（Fabric/Quilt 自身目录无 jar），主类 net.minecraft.client.main.Main 所在 */
    if (!uses_module_path) {
        const wchar_t* jv = v->jar_id ? v->jar_id : v->id;
        wchar_t* cv = path_join(gamedir, L"versions");
        wchar_t* cvd = path_join(cv, jv);
        wchar_t* cjn = wcat3(jv, L".jar", L"");
        wchar_t* cj = path_join(cvd, cjn);
        free(cjn);
        if (file_exists(cj)) {
            wsb_add(&cp, cj);
        } else {
            missing++;
        }
        free(cj);
        free(cv); free(cvd);
    }
    for (size_t i = 0; i < v->nlibs; i++) {
        if (v->libs[i].is_native) continue;
        if (!v->libs[i].present) {
            missing++;
            if (missing <= 5) {
                wchar_t tmp[512];
                _snwprintf_s(tmp, 512, _TRUNCATE, L"缺少库: %s", v->libs[i].name);
                /* keep log simple: appended below */
                (void)tmp;
            }
            continue;
        }
        if (cp.n) wsb_addc(&cp, L';');
        wsb_add(&cp, v->libs[i].fullpath);
    }
    r->missing_libs = missing;
    if (cp.n == 0) {
        _snwprintf_s(r->err, 640, _TRUNCATE,
                     L"classpath 为空：该版本的库文件全部缺失。\n请先安装/补齐游戏文件（第一版不做下载）。");
        wsb_free(&cp);
        free(natives_target); free(natives); free(vdir); free(game_dir_eff);
        return 0;
    }
    wchar_t* classpath = wsb_take(&cp);
    wsb_free(&cp);

    wchar_t* assets_root = path_join(gamedir, L"assets");
    wchar_t* lib_dir = path_join(gamedir, L"libraries");

    subst_ctx sc;
    memset(&sc, 0, sizeof(sc));
    sc.player = player;
    sc.uuid = uuid;
    sc.token = token;
    sc.user_type = online ? L"msa" : L"legacy";
    sc.version_name = v->id;
    sc.version_type = v->version_type ? v->version_type : L"release";
    sc.game_dir = game_dir_eff;
    sc.assets_root = assets_root;
    sc.assets_index = v->assets ? v->assets : L"legacy";
    sc.natives = natives;
    sc.classpath = classpath;
    sc.lib_dir = lib_dir;
    sc.launcher_name = L"LLcl";
    sc.launcher_version = LLCL_VERSION;
    sc.auth_session = token;
    sc.xuid = online ? xuid : NULL;
    sc.server_ip = server_ip ? server_ip : L"";

    int heap = mem_mb > 0 ? mem_mb : auto_heap_mb();
    int xms = auto_xms_mb(heap);

    /* 3. assemble args */
    wsa args;
    memset(&args, 0, sizeof(args));
    args_push(&args, java_exe);
    {
        wchar_t mb[32];
        _snwprintf_s(mb, 32, _TRUNCATE, L"-Xmx%dM", heap);
        args_push(&args, mb);
        _snwprintf_s(mb, 32, _TRUNCATE, L"-Xms%dM", xms);
        args_push(&args, mb);
    }
    /* 运行内存优化：G1GC 低停顿参数（紧跟内存参数，按版本/堆自适应） */
    if (mem_opt) jvm_mem_flags(heap, v->java_major, &args);
    if (v->has_modern) {
        for (size_t i = 0; i < v->jvm.n; i++) {
            wchar_t* s = subst_str(v->jvm.v[i], &sc);
            if (s && *s) args_push(&args, s);
            free(s);
        }
        if (v->main_class) args_push(&args, v->main_class);
        for (size_t i = 0; i < v->game.n; i++) {
            wchar_t* s = subst_str(v->game.v[i], &sc);
            if (s && *s) args_push(&args, s);
            free(s);
        }
        /* 多人服务器直连参数（1.6+ 支持；老版本会忽略未知参数） */
        add_server_args(&args, server_ip);
    } else if (v->has_legacy) {
        {
            wchar_t* a = wcat3(L"-Djava.library.path=", natives, L"");
            args_push(&args, a);
            free(a);
        }
        args_push(&args, L"-cp");
        args_push(&args, classpath);
        /* forge-friendly flags for legacy versions */
        args_push(&args, L"-Dfml.ignoreInvalidMinecraftCertificates=true");
        args_push(&args, L"-Dfml.ignorePatchDiscrepancies=true");
        if (v->main_class) args_push(&args, v->main_class);
        /* split minecraftArguments template on spaces, then substitute */
        wchar_t* tmp = wdup(v->legacy);
        wchar_t* ctx = NULL;
        wchar_t* tok = wcstok_s(tmp, L" ", &ctx);
        while (tok) {
            if (*tok) {
                wchar_t* s = subst_str(tok, &sc);
                if (s && *s) args_push(&args, s);
                free(s);
            }
            tok = wcstok_s(NULL, L" ", &ctx);
        }
        free(tmp);
        add_server_args(&args, server_ip);
    } else {
        _snwprintf_s(r->err, 640, _TRUNCATE, L"版本 %s 没有可用的启动参数（arguments/minecraftArguments 均缺失）", v->id);
        wsa_free(&args);
        free(classpath); free(assets_root); free(lib_dir);
        free(natives_target); free(natives); free(vdir); free(game_dir_eff);
        return 0;
    }

    /* 4. command line, with @argfile fallback for >30K */
    size_t clen = 0;
    wchar_t* cmdline = join_cmdline(&args, &clen);
    wchar_t* argfile = NULL;
    /* 调试：LLCL_DEBUG_CMD=路径 时把最终命令行写入文件 */
    {
        wchar_t dbg[4096];
        if (GetEnvironmentVariableW(L"LLCL_DEBUG_CMD", dbg, 4096) > 0 && *dbg) {
            FILE* df = NULL;
            _wfopen_s(&df, dbg, L"wb");
            if (df) {
                char* u8 = w2u(cmdline);
                if (u8) { fwrite(u8, 1, strlen(u8), df); free(u8); }
                fclose(df);
            }
        }
    }
    if (clen > CMDLINE_MAX) {
        argfile = write_argfile(vdir, &args);
        if (argfile) {
            wsa a2;
            memset(&a2, 0, sizeof(a2));
            args_push(&a2, java_exe);
            wchar_t* at = wcat3(L"@", argfile, L"");
            args_push(&a2, at);
            free(at);
            free(cmdline);
            cmdline = join_cmdline(&a2, &clen);
            wsa_free(&a2);
            if (clen > CMDLINE_MAX) {
                _snwprintf_s(r->err, 640, _TRUNCATE, L"命令行仍然过长（%u 字符）", (unsigned)clen);
                free(cmdline); wsa_free(&args);
                free(classpath); free(assets_root); free(lib_dir);
                free(natives_target); free(natives); free(vdir); free(game_dir_eff); free(argfile);
                return 0;
            }
        }
    }
    r->us_build = now_us() - t0;

    /* 5. spawn — java stdout/stderr 重定向到 <gamedir>\logs\llcl-launch.log，
       并观察 5 秒：若进程立即退出则记录退出码（秒退=启动异常，报错在日志里） */
    wchar_t* logs_dir = path_join(gamedir, L"logs");
    if (!dir_exists(logs_dir)) ensure_dir_rec(logs_dir);
    wchar_t* logfile = path_join(logs_dir, L"llcl-launch.log");
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(logfile, FILE_APPEND_DATA | FILE_WRITE_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE hIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = (hIn  != INVALID_HANDLE_VALUE) ? hIn  : NULL;
    si.hStdOutput = (hLog != INVALID_HANDLE_VALUE) ? hLog : NULL;
    si.hStdError  = (hLog != INVALID_HANDLE_VALUE) ? hLog : NULL;
    if (hLog == INVALID_HANDLE_VALUE && hIn == INVALID_HANDLE_VALUE) si.dwFlags = 0;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    /* 启动游戏前释放启动器自身占用（压缩堆 + 换出工作集），把物理内存尽量让给游戏 */
    if (mem_opt) mem_trim();
    BOOL okc = CreateProcessW(java_exe, cmdline, NULL, NULL,
                              (hLog != INVALID_HANDLE_VALUE || hIn != INVALID_HANDLE_VALUE) ? TRUE : FALSE,
                              CREATE_NO_WINDOW, NULL, game_dir_eff, &si, &pi);
    r->us_spawn = now_us() - t0;
    r->us_total = now_us() - t0;
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (hIn  != INVALID_HANDLE_VALUE) CloseHandle(hIn);
    if (!okc) {
        wchar_t msg[512];
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, GetLastError(), 0, msg, 512, NULL);
        _snwprintf_s(r->err, 640, _TRUNCATE, L"CreateProcessW 失败（错误 %lu: %s）", GetLastError(), msg);
        free(cmdline); wsa_free(&args);
        free(classpath); free(assets_root); free(lib_dir);
        free(natives_target); free(natives); free(vdir); free(game_dir_eff); free(argfile);
        free(logs_dir); free(logfile);
        return 0;
    }
    r->ok = 1;
    r->pid = pi.dwProcessId;
    /* 观察窗口 5 秒：子进程若自行退出，视为启动异常（缺类/缺库/参数错等），记录退出码 */
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        r->early_exit = 1;
        r->exit_code = ec;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    _snwprintf_s(r->log_path, 512, _TRUNCATE, L"%s", logfile);
    free(logs_dir);
    free(logfile);

    free(cmdline);
    if (argfile) {
        DeleteFileW(argfile); /* argfile already consumed by the JVM */
        free(argfile);
    }
    wsa_free(&args);
    free(classpath); free(assets_root); free(lib_dir);
    free(natives_target); free(natives); free(vdir); free(game_dir_eff);
    return 1;
}
