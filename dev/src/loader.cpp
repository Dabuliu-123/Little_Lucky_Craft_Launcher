/* LLcl - loader.cpp : 模组加载器版本列表与独立安装。
 * Fabric 走 BMCLAPI fabric-meta + /maven 镜像（国内快），失败回退官方；
 * Quilt 走官方 meta.quiltmc.org（BMCLAPI 未代理 quilt-meta）；
 * Forge/NeoForge 走 BMCLAPI：下载 installer 并以 --installClient 运行
 * （installer 负责下载库、patch 客户端、注入 profile，无法只下 profile）。
 */
#include "llcl.h"
#define JSMN_HEADER
#include "jsmn.h"

const wchar_t* loader_kind_name(ld_kind k) {
    switch (k) {
    case LD_QUILT:    return L"Quilt";
    case LD_FORGE:    return L"Forge";
    case LD_NEOFORGE: return L"NeoForge";
    default:          return L"Fabric";
    }
}

/* 判断是否为加载器版本（id 含加载器名） */
static int ld_is_loader_version(const wchar_t* id) {
    return wcsstr(id, L"fabric") || wcsstr(id, L"quilt") ||
           wcsstr(id, L"forge") || wcsstr(id, L"neoforge");
}

/* g:a:v[:c] → 库相对路径（group/artifact/version/artifact-version[-classifier].jar） */
static wchar_t* name_to_rel(const wchar_t* name) {
    wchar_t* nm = wdup(name);
    wchar_t* cols[4] = { NULL, NULL, NULL, NULL };
    int nc = 0;
    wchar_t* s = nm;
    cols[nc++] = s;
    while (*s && nc < 4) { if (*s == L':') { *s = 0; cols[nc++] = s + 1; } s++; }
    if (nc < 3) { free(nm); return NULL; }
    wchar_t* g = cols[0]; wchar_t* a = cols[1]; wchar_t* v = cols[2];
    wchar_t* c = (nc >= 4) ? cols[3] : NULL;
    wsb b; wsb_init(&b);
    for (wchar_t* p = g; *p; p++) wsb_addc(&b, *p == L'.' ? L'/' : *p);
    wsb_addf(&b, L"/%s/%s/%s-%s", a, v, a, v);
    if (c) wsb_addf(&b, L"-%s", c);
    wsb_add(&b, L".jar");
    wchar_t* r = wsb_take(&b);
    wsb_free(&b);
    free(nm);
    return r;
}

/* 依次尝试候选 URL，返回第一个 HTTP 200 的响应（调用方 free *data） */
static int get_first(HINTERNET inet, const wchar_t* const* urls, int nurls,
                     unsigned char** data, size_t* dlen) {
    for (int i = 0; i < nurls; i++) {
        unsigned char* d = NULL; size_t dl = 0; int st = 0;
        if (net_get(inet, urls[i], &d, &dl, &st) && st == 200 && d) {
            *data = d; *dlen = dl; return 1;
        }
        free(d);
    }
    return 0;
}

/* 把 in 中所有 find 替换为 repl，返回新串（调用方 free） */
static char* str_replace_all(const char* in, const char* find, const char* repl) {
    size_t fl = strlen(find), rl = strlen(repl);
    if (fl == 0) return udup(""); /* unused, keeps size math safe */
    int count = 0;
    const char* p = in;
    while ((p = strstr(p, find)) != NULL) { count++; p += fl; }
    /* rl - fl can be negative (自定义版本名比原版名短)，必须用有符号差
       再与 count 相乘，否则 size_t 下溢会申请一个巨大的缓冲区。 */
    ptrdiff_t delta = (ptrdiff_t)count * ((ptrdiff_t)rl - (ptrdiff_t)fl);
    size_t total = strlen(in);
    if (delta > 0) total += (size_t)delta;
    else if ((size_t)(-delta) > total) total = 1; /* impossible in practice */
    else total -= (size_t)(-delta);
    char* out = (char*)xalloc(total + 1);
    char* o = out;
    const char* s = in;
    const char* m;
    while ((m = strstr(s, find)) != NULL) {
        size_t pre = (size_t)(m - s);
        memcpy(o, s, pre); o += pre;
        memcpy(o, repl, rl); o += rl;
        s = m + fl;
    }
    strcpy(o, s);
    return out;
}

/* ---------------- 版本号比较（排序，最新在前） ---------------- */
static int segcmp(const wchar_t* a, int alen, const wchar_t* b, int blen) {
    int an = alen > 0, bn = blen > 0;
    for (int i = 0; i < alen; i++) if (!iswdigit(a[i])) { an = 0; break; }
    for (int i = 0; i < blen; i++) if (!iswdigit(b[i])) { bn = 0; break; }
    if (an && bn) {
        long x = 0, y = 0;
        for (int i = 0; i < alen; i++) x = x * 10 + (a[i] - L'0');
        for (int i = 0; i < blen; i++) y = y * 10 + (b[i] - L'0');
        if (x < y) return -1; if (x > y) return 1; return 0;
    }
    int n = alen < blen ? alen : blen;
    for (int i = 0; i < n; i++) { if (a[i] < b[i]) return -1; if (a[i] > b[i]) return 1; }
    if (alen < blen) return -1; if (alen > blen) return 1; return 0;
}
static int vercmp(const wchar_t* a, const wchar_t* b) {
    while (*a && *b) {
        const wchar_t* ae = a; while (*ae && *ae != L'.' && *ae != L'-' && *ae != L'+') ae++;
        const wchar_t* be = b; while (*be && *be != L'.' && *be != L'-' && *be != L'+') be++;
        int c = segcmp(a, (int)(ae - a), b, (int)(be - b));
        if (c) return c;
        a = *ae ? ae + 1 : ae; b = *be ? be + 1 : be;
    }
    if (*a) return 1; if (*b) return -1; return 0;
}
static int __cdecl ld_entry_cmp(const void* x, const void* y) {
    /* 降序：最新在前 */
    return vercmp(((const ld_entry*)y)->version, ((const ld_entry*)x)->version);
}

/* MC 版本 → NeoForge 版本前缀（1.20.4→"20.4"，1.21→"21.0"，1.21.1→"21.1"） */
static wchar_t* neoforge_prefix(const wchar_t* mcver) {
    const wchar_t* dot = wcschr(mcver, L'.');
    if (!dot) return NULL;
    wchar_t* rest = wdup(dot + 1);   /* 第一个点之后，如 "20.4" / "21" */
    if (!wcschr(rest, L'.')) {
        /* 两段版本（1.21）→ 补 .0 */
        wchar_t* out = wcat3(rest, L".0", L"");
        free(rest);
        return out;
    }
    return rest;
}

/* ---------------- 加载器版本列表 ---------------- */
/* Forge：BMCL /forge/minecraft/<mcver>，JSON 数组（version/build/fixed） */
static int forge_list(HINTERNET inet, const wchar_t* mcver, ld_entry** out, int* n,
                      wchar_t* err, size_t errlen) {
    wchar_t url[320];
    _snwprintf_s(url, 320, _TRUNCATE,
                 L"https://bmclapi2.bangbang93.com/forge/minecraft/%s", mcver);
    unsigned char* data = NULL; size_t dlen = 0; int st = 0;
    if (!net_get(inet, url, &data, &dlen, &st) || st != 200 || !data) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"无法获取 Forge 版本列表（该 MC 版本可能不支持）");
        return 0;
    }
    const char* js = (const char*)data;
    jsmntok_t* t = NULL; int nt = 0;
    if (!json_parse_mem(js, dlen, (struct jsmntok**)&t, &nt) || t[0].type != JSMN_ARRAY) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"Forge 版本列表格式错误");
        free(t); free(data); return 0;
    }
    ld_entry* arr = NULL; int cnt = 0;
    const jsmntok_t* p = t + 1;
    for (int i = 0; i < t[0].size; i++) {
        const jsmntok_t* el = p; p = tok_skip(el);
        if (el->type != JSMN_OBJECT) continue;
        const jsmntok_t* vt = jm(js, el, "version");
        if (!vt || vt->type != JSMN_STRING) continue;
        wchar_t* ver = jwstrdup(js, vt);
        /* fixed 字段非空表示该版本被后续修复版替代；仍列出，但标记非稳定。
         * Forge 版本号含 "beta" 为测试版。 */
        int stable = ver && !wcsstr(ver, L"beta");
        /* 去重（同版本号可能因 fixed 出现多次） */
        int dup = 0;
        for (int k = 0; k < cnt; k++) if (weq(arr[k].version, ver)) { dup = 1; break; }
        if (!dup) {
            arr = (ld_entry*)xrealloc(arr, (cnt + 1) * sizeof(ld_entry));
            arr[cnt].version = ver; arr[cnt].stable = stable; cnt++;
        } else free(ver);
    }
    free(t); free(data);
    if (!cnt) { _snwprintf_s(err, errlen, _TRUNCATE, L"未解析到 Forge 版本"); return 0; }
    qsort(arr, cnt, sizeof(ld_entry), ld_entry_cmp);
    *out = arr; *n = cnt;
    return 1;
}

/* NeoForge：BMCL maven metadata，按 MC 版本前缀过滤 */
static int neoforge_list(HINTERNET inet, const wchar_t* mcver, ld_entry** out, int* n,
                         wchar_t* err, size_t errlen) {
    wchar_t* prefix = neoforge_prefix(mcver);
    if (!prefix) { _snwprintf_s(err, errlen, _TRUNCATE, L"该 MC 版本不支持 NeoForge"); return 0; }
    const wchar_t* url = L"https://bmclapi2.bangbang93.com/maven/net/neoforged/neoforge/maven-metadata.xml";
    unsigned char* data = NULL; size_t dlen = 0; int st = 0;
    if (!net_get(inet, url, &data, &dlen, &st) || st != 200 || !data) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"无法获取 NeoForge 版本列表");
        free(prefix); return 0;
    }
    char* pfx = w2u(prefix);
    size_t pfl = strlen(pfx);
    ld_entry* arr = NULL; int cnt = 0;
    const char* p = (const char*)data;
    const char* f;
    while ((f = strstr(p, "<version>")) != NULL) {
        f += 9;
        const char* e = strstr(f, "</version>");
        if (!e) break;
        size_t vl = e - f;
        /* 前缀匹配后，下一个字符必须是版本分隔符 '.'，避免 "21.1" 误匹配 "21.11"/"21.10" */
        if (vl >= pfl && !strncmp(f, pfx, pfl) && (f[pfl] == '.' || f[pfl] == 0)) {
            char* v = (char*)xalloc(vl + 1); memcpy(v, f, vl); v[vl] = 0;
            int stable = !strstr(v, "beta") && !strstr(v, "alpha");
            arr = (ld_entry*)xrealloc(arr, (cnt + 1) * sizeof(ld_entry));
            arr[cnt].version = u2w(v); arr[cnt].stable = stable; cnt++;
            free(v);
        }
        p = e + 10;
    }
    free(data); free(pfx); free(prefix);
    if (!cnt) { _snwprintf_s(err, errlen, _TRUNCATE, L"该 MC 版本无对应 NeoForge（NeoForge 自 1.20.2 起）"); return 0; }
    qsort(arr, cnt, sizeof(ld_entry), ld_entry_cmp);
    *out = arr; *n = cnt;
    return 1;
}

int loader_list(ld_kind kind, const wchar_t* mcver, ld_entry** out, int* n,
                wchar_t* err, size_t errlen) {
    *out = NULL; *n = 0;
    HINTERNET inet;
    if (!net_init(&inet)) { _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败"); return 0; }

    int rc;
    if (kind == LD_FORGE)       rc = forge_list(inet, mcver, out, n, err, errlen);
    else if (kind == LD_NEOFORGE) rc = neoforge_list(inet, mcver, out, n, err, errlen);
    else {
        /* Fabric/Quilt：meta 接口 */
        wchar_t u_bmcl[512], u_off[512];
        const wchar_t* urls[2]; int nu = 0;
        if (kind == LD_QUILT) {
            _snwprintf_s(u_off, 512, _TRUNCATE, L"https://meta.quiltmc.org/v3/versions/loader/%s", mcver);
            urls[nu++] = u_off;
        } else {
            _snwprintf_s(u_bmcl, 512, _TRUNCATE,
                         L"https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/%s", mcver);
            _snwprintf_s(u_off, 512, _TRUNCATE, L"https://meta.fabricmc.net/v2/versions/loader/%s", mcver);
            urls[nu++] = u_bmcl; urls[nu++] = u_off;
        }
        unsigned char* data = NULL; size_t dlen = 0;
        rc = get_first(inet, urls, nu, &data, &dlen);
        if (rc) {
            const char* js = (const char*)data;
            jsmntok_t* t = NULL; int nt = 0;
            if (json_parse_mem(js, dlen, (struct jsmntok**)&t, &nt) && t[0].type == JSMN_ARRAY) {
                ld_entry* arr = NULL; int cnt = 0;
                const jsmntok_t* q = t + 1;
                for (int i = 0; i < t[0].size; i++) {
                    const jsmntok_t* el = q; q = tok_skip(el);
                    if (el->type != JSMN_OBJECT) continue;
                    const jsmntok_t* L = jm(js, el, "loader");
                    if (!L) continue;
                    const jsmntok_t* vt = jm(js, L, "version");
                    if (!vt || vt->type != JSMN_STRING) continue;
                    ld_entry e;
                    e.version = jwstrdup(js, vt);
                    const jsmntok_t* stb = jm(js, L, "stable");
                    e.stable = stb ? jbool(js, stb) : 0;
                    arr = (ld_entry*)xrealloc(arr, (cnt + 1) * sizeof(ld_entry));
                    arr[cnt++] = e;
                }
                if (cnt) { qsort(arr, cnt, sizeof(ld_entry), ld_entry_cmp); *out = arr; *n = cnt; }
                else { _snwprintf_s(err, errlen, _TRUNCATE, L"未解析到任何加载器版本"); rc = 0; }
            } else {
                _snwprintf_s(err, errlen, _TRUNCATE, L"加载器版本列表格式错误"); rc = 0;
            }
            free(t); free(data);
        } else {
            _snwprintf_s(err, errlen, _TRUNCATE,
                         L"无法获取 %s 加载器版本列表（网络不可用，或该游戏版本不被支持）",
                         loader_kind_name(kind));
        }
    }
    InternetCloseHandle(inet);
    return rc;
}

void loader_list_free(ld_entry* e, int n) {
    if (!e) return;
    for (int i = 0; i < n; i++) free(e[i].version);
    free(e);
}

/* ---------------- 找/下载运行 installer 的 Java ---------------- */
static wchar_t* ensure_java(int major, dl_progress* prog, wchar_t* err, size_t errlen) {
    wchar_t* j = javadl_find(major, 1);
    if (j) return j;
    javaitem* jj = NULL; int jn = 0;
    if (detect_javas(&jj, &jn)) {
        /* 优先精确匹配，否则选">=需求"中 major 最小且为 64 位的（Java 向后兼容） */
        int best = -1, bestMajor = -1;
        for (int i = 0; i < jn; i++) {
            if (!jj[i].is64) continue;
            if (jj[i].major < major) continue;
            if (jj[i].major == major) {
                wchar_t* p = wdup(jj[i].path);
                javas_free(jj, jn);
                return p;
            }
            if (best < 0 || jj[i].major < bestMajor) { best = i; bestMajor = jj[i].major; }
        }
        if (best >= 0) {
            wchar_t* p = wdup(jj[best].path);
            javas_free(jj, jn);
            return p;
        }
    }
    javas_free(jj, jn);
    if (!javadl_install(major, 1, prog, err, errlen)) return NULL;
    return javadl_find(major, 1);
}

/* 确保 launcher_profiles.json 存在（Forge/NeoForge installer 强制要求） */
static void ensure_launcher_profiles(const wchar_t* gamedir) {
    wchar_t* lp = path_join(gamedir, L"launcher_profiles.json");
    if (!file_exists(lp)) {
        const char* body = "{\"profiles\":{},\"clientToken\":\"00000000-0000-0000-0000-000000000000\",\"selectedProfile\":\"\"}";
        FILE* f = _wfopen(lp, L"wb");
        if (f) { fwrite(body, 1, strlen(body), f); fclose(f); }
    }
    free(lp);
}

/* 运行 installer（--installClient），输出重定向到临时文件，等待完成。
 * 返回 1 成功（日志含 "Successfully installed" 或退出码 0）。 */
static int run_installer(const wchar_t* java, const wchar_t* installer,
                         const wchar_t* gamedir, const wchar_t* mirror,
                         dl_progress* prog, wchar_t* err, size_t errlen) {
    wchar_t* logs = path_join(gamedir, L"logs");
    ensure_dir_rec(logs);
    wchar_t* outfile = path_join(logs, L"llcl-installer.log");
    free(logs);
    SECURITY_ATTRIBUTES sa; memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(outfile, FILE_APPEND_DATA | FILE_WRITE_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE hIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    /* 命令行（CreateProcessW 以 lpApplicationName 启动时，lpCommandLine 仍需以
     * 程序名作为第一个 token，否则 -jar 会被当作 argv[0]）：
     * "<java>" -jar installer --installClient gamedir --mirror mirror（mirror 带尾斜杠） */
    wchar_t* cmd = NULL;
    size_t need = wcslen(java) + wcslen(installer) + wcslen(gamedir) + wcslen(mirror) + 160;
    cmd = (wchar_t*)xalloc(need * sizeof(wchar_t));
    _snwprintf_s(cmd, need, _TRUNCATE,
                 L"\"%s\" -jar \"%s\" --installClient \"%s\" --mirror \"%s\"",
                 java, installer, gamedir, mirror);
    STARTUPINFOW si; memset(&si, 0, sizeof(si));
    si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hIn; si.hStdOutput = hLog; si.hStdError = hLog;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(java, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, gamedir, &si, &pi);
    free(cmd);
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (hIn != INVALID_HANDLE_VALUE) CloseHandle(hIn);
    if (!ok) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"无法启动 installer（Java 运行失败）");
        free(outfile);
        return 0;
    }
    /* 轮询等待（installer 下载库 + patch，可能数分钟），周期性更新状态 */
    if (prog) { wcsncpy_s(prog->cur, 256, L"正在运行安装器（下载库/补丁，可能需数分钟）", _TRUNCATE); }
    DWORD ec = 1;
    for (int waited = 0; ; waited += 2000) {
        DWORD wr = WaitForSingleObject(pi.hProcess, 2000);
        if (wr == WAIT_OBJECT_0) { GetExitCodeProcess(pi.hProcess, &ec); break; }
        if (waited >= 600000) { TerminateProcess(pi.hProcess, 1); ec = 1; break; } /* 10 分钟超时 */
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    /* 读取 installer 日志判断成功 */
    int success = (ec == 0);
    char* ilog = NULL;
    FILE* f = _wfopen(outfile, L"rb");
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0) {
            ilog = (char*)xalloc((size_t)sz + 1);
            sz = (long)fread(ilog, 1, (size_t)sz, f); ilog[sz] = 0;
        }
        fclose(f);
    }
    if (ilog && strstr(ilog, "Successfully installed")) success = 1;
    if (!success && ilog) {
        /* 取最后一段错误信息（粗略） */
        const char* tail = ilog + strlen(ilog);
        const char* start = tail - 400; if (start < ilog) start = ilog;
        while (start > ilog && *start != '\n') start--;
        char* u8 = (char*)xalloc(strlen(start) + 1); strcpy_s(u8, strlen(start) + 1, start);
        wchar_t* we = u2w(u8);
        _snwprintf_s(err, errlen, _TRUNCATE, L"安装器运行失败：%s", we ? we : L"未知错误");
        free(we); free(u8);
    }
    free(ilog); free(outfile);
    return success;
}

/* 把官方 maven 库 URL 改写为 BMCL 镜像（国内可达），返回新宽串（调用方 free）。
 * 未命中已知官方域名时原样返回（可能本就可达，或已是镜像）。 */
static wchar_t* lib_url_to_bmcl(const wchar_t* url) {
    static const struct { const wchar_t* from; const wchar_t* to; } map[] = {
        { L"https://maven.neoforged.net/releases/", L"https://bmclapi2.bangbang93.com/maven/" },
        { L"https://maven.minecraftforge.net/",     L"https://bmclapi2.bangbang93.com/maven/" },
        { L"https://files.minecraftforge.net/maven/", L"https://bmclapi2.bangbang93.com/maven/" },
        { L"https://repo1.maven.org/maven2/",      L"https://bmclapi2.bangbang93.com/maven/" },
        { L"https://libraries.minecraft.net/",    L"https://bmclapi2.bangbang93.com/maven/" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        size_t fl = wcslen(map[i].from);
        if (wcslen(url) >= fl && _wcsnicmp(url, map[i].from, fl) == 0)
            return wcat3(map[i].to, url + fl, L"");
    }
    return wdup(url);
}

/* 预下载 installer 所需的全部库到 libraries/。installer 运行时会跳过已存在的库，
 * 从而避免它直连被墙的 maven.neoforged.net / maven.minecraftforge.net。
 * 返回 1 成功（或无库可下），0 失败。 */
static int prefetch_installer_libs(const wchar_t* gamedir, const wchar_t* instPath,
                                   dl_progress* prog, wchar_t* err, size_t errlen) {
    size_t pflen = 0;
    unsigned char* pf = zip_read_file(instPath, "install_profile.json", &pflen);
    if (!pf) { _snwprintf_s(err, errlen, _TRUNCATE, L"无法读取安装器配置"); return 0; }
    jsmntok_t* t = NULL; int nt = 0;
    const char* js = (const char*)pf;
    if (!json_parse_mem(js, pflen, (struct jsmntok**)&t, &nt) || t[0].type != JSMN_OBJECT) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"安装器配置格式错误");
        free(t); free(pf); return 0;
    }
    const jsmntok_t* libs = jm(js, t, "libraries");
    if (!libs || libs->type != JSMN_ARRAY) { free(t); free(pf); return 1; }
    wchar_t* libroot = path_join(gamedir, L"libraries");
    dl_job* jobs = NULL; size_t jn = 0;
    const jsmntok_t* el = libs + 1;
    for (int i = 0; i < libs->size; i++) {
        const jsmntok_t* cur = el; el = tok_skip(el);
        if (cur->type != JSMN_OBJECT) continue;
        const jsmntok_t* dl = jm(js, cur, "downloads");
        if (!dl) continue;
        const jsmntok_t* art = jm(js, dl, "artifact");
        if (!art) continue;
        const jsmntok_t* ut = jm(js, art, "url");
        const jsmntok_t* pt = jm(js, art, "path");
        if (!ut || !pt) continue;
        wchar_t* url = jwstrdup(js, ut);
        wchar_t* rel = jwstrdup(js, pt);
        wchar_t* dest = path_join(libroot, rel);
        if (!file_exists(dest)) {
            wchar_t* bmcl = lib_url_to_bmcl(url);
            jobs = (dl_job*)xrealloc(jobs, (jn + 1) * sizeof(dl_job));
            jobs[jn].url = bmcl; jobs[jn].dest = dest; jobs[jn].name = wdup(rel);
            jobs[jn].size = 0;
            jn++;
        } else {
            free(dest);
        }
        free(url); free(rel);
    }
    int rc = 1;
    if (jn) {
        if (prog) { wcsncpy_s(prog->cur, 256, L"预下载安装器依赖库", _TRUNCATE); }
        rc = dl_run_jobs(jobs, jn, prog, err, errlen);
        for (size_t k = 0; k < jn; k++) {
            free(jobs[k].url); free(jobs[k].dest); free(jobs[k].name);
        }
    }
    free(jobs); free(libroot); free(t); free(pf);
    return rc;
}

/* Forge/NeoForge：下载 installer 并运行 */
static int forge_install(ld_kind kind, const wchar_t* gamedir, const wchar_t* mcver,
                         const wchar_t* dir_id, const wchar_t* loader, dl_progress* prog,
                         wchar_t* err, size_t errlen) {
    HINTERNET inet;
    if (!net_init(&inet)) { _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败"); return 0; }
    wchar_t url[512], instRel[512];
    if (kind == LD_FORGE) {
        _snwprintf_s(url, 512, _TRUNCATE,
                     L"https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge/%s-%s/forge-%s-%s-installer.jar",
                     mcver, loader, mcver, loader);
        _snwprintf_s(instRel, 512, _TRUNCATE,
                     L"net/minecraftforge/forge/%s-%s/forge-%s-%s-installer.jar",
                     mcver, loader, mcver, loader);
    } else {
        _snwprintf_s(url, 512, _TRUNCATE,
                     L"https://bmclapi2.bangbang93.com/maven/net/neoforged/neoforge/%s/neoforge-%s-installer.jar",
                     loader, loader);
        _snwprintf_s(instRel, 512, _TRUNCATE,
                     L"net/neoforged/neoforge/%s/neoforge-%s-installer.jar", loader, loader);
    }
    /* installer 下载到 libraries 下对应位置（复用，不重复下载） */
    wchar_t* libroot = path_join(gamedir, L"libraries");
    wchar_t* instPath = path_join(libroot, instRel);
    free(libroot);
    if (!file_exists(instPath)) {
        /* 创建 installer 的父目录（instPath 含混合分隔符，找最后一个 \ 或 /） */
        wchar_t* parent = wdup(instPath);
        wchar_t* ps = wcsrchr(parent, L'\\');
        wchar_t* fs = wcsrchr(parent, L'/');
        wchar_t* slash = ps;
        if (fs && (!slash || fs > slash)) slash = fs;
        if (slash) { *slash = 0; ensure_dir_rec(parent); }
        free(parent);
        if (prog) { wcsncpy_s(prog->cur, 256, L"下载安装器", _TRUNCATE); }
        /* 流式写盘，避免把 10-30MB 的 installer 整个读进内存。 */
        if (!net_download_file(inet, url, instPath, NULL, NULL) || !file_exists(instPath)) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"下载安装器失败（该版本可能不存在）");
            _wremove(instPath);
            free(instPath); InternetCloseHandle(inet); return 0;
        }
        /* 安装器必须是 zip/jar；镜像错误页会以 200 返回 HTML。 */
        FILE* chk = _wfopen(instPath, L"rb");
        char magic[2] = { 0, 0 };
        if (chk) { fread(magic, 1, 2, chk); fclose(chk); }
        if (!(magic[0] == 'P' && magic[1] == 'K')) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"安装器文件无效（镜像返回了错误页）");
            _wremove(instPath);
            free(instPath); InternetCloseHandle(inet); return 0;
        }
    }
    InternetCloseHandle(inet);

    /* 预下载 installer 依赖库（换 BMCL 镜像），installer 运行时直接命中缓存，
     * 避免它直连被墙的 maven.neoforged.net / maven.minecraftforge.net */
    if (!prefetch_installer_libs(gamedir, instPath, prog, err, errlen)) return 0;

    /* 运行 installer 需要的 Java：从实际游戏目录（dir_id，自定义版本）的 JSON 读 major */
    int major = 0;
    {
        wchar_t* vj = path_join3(gamedir, L"versions", dir_id);
        wchar_t* vjp = path_join(vj, wcat3(dir_id, L".json", L""));
        char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
        if (json_load_file(vjp, &js, &t, &nt) && js) {
            const jsmntok_t* jr = jm(js, t, "javaVersion");
            if (jr) {
                /* 官方字段是 majorVersion；旧代码读 "major"，永远读不到，
                   导致 Forge 1.20.1+ 用 Java 8 跑安装器而失败。 */
                const jsmntok_t* ma = jm(js, jr, "majorVersion");
                if (!ma) ma = jm(js, jr, "major");
                if (ma && ma->type == JSMN_PRIMITIVE) major = (int)strtol(js + ma->start, NULL, 10);
            }
            json_free(js, t);
        }
        free(vj); free(vjp);
    }
    if (major <= 0) major = 8;
    if (prog) { wcsncpy_s(prog->cur, 256, L"准备 Java 环境", _TRUNCATE); }
    wchar_t* java = ensure_java(major, prog, err, errlen);
    if (!java) { free(instPath); return 0; }

    ensure_launcher_profiles(gamedir);
    const wchar_t* mirror = L"https://bmclapi2.bangbang93.com/maven/";
    int rc = run_installer(java, instPath, gamedir, mirror, prog, err, errlen);
    free(java); free(instPath);
    /* 自定义版本：installer 生成的加载器版本 inheritsFrom 原版 mcver，但游戏目录
     * 只有 dir_id。扫描 versions 下新生成的版本，把 inheritsFrom 从 mcver 改为 dir_id。 */
    if (rc && dir_id && !weq(dir_id, mcver)) {
        wchar_t* vroot = path_join(gamedir, L"versions");
        wchar_t* found[64]; int fn = 0;
        WIN32_FIND_DATAW fd;
        wchar_t* pat = path_join(vroot, L"*");
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == L'.') continue;
                if (ld_is_loader_version(fd.cFileName)) {
                    wchar_t* vj = path_join(vroot, fd.cFileName);
                    wchar_t* vjp = path_join(vj, wcat3(fd.cFileName, L".json", L""));
                    char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
                    if (json_load_file(vjp, &js, &t, &nt) && js) {
                        const jsmntok_t* ih = jm(js, t, "inheritsFrom");
                        if (ih && ih->type == JSMN_STRING) {
                            wchar_t* cur = jwstrdup(js, ih);
                            if (cur && weq(cur, mcver)) {
                                /* 用 dir_id 替换该 token 范围 */
                                char* dirU = w2u(dir_id);
                                size_t total = ih->start + strlen(dirU) +
                                               (strlen(js) - ih->end) + 1;
                                char* o = (char*)xalloc(total);
                                char* pp = o;
                                memcpy(pp, js, ih->start); pp += ih->start;
                                memcpy(pp, dirU, strlen(dirU)); pp += strlen(dirU);
                                memcpy(pp, js + ih->end, strlen(js) - ih->end);
                                pp += strlen(js) - ih->end; *pp = 0;
                                FILE* wf = _wfopen(vjp, L"wb");
                                if (wf) { fwrite(o, 1, strlen(o), wf); fclose(wf); }
                                free(o); free(dirU);
                            }
                            free(cur);
                        }
                        json_free(js, t);
                    }
                    free(vj); free(vjp);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        free(pat); free(vroot);
        for (int i = 0; i < fn; i++) free(found[i]);
    }
    return rc;
}

/* ---------------- 安装加载器 ---------------- */
int loader_install(ld_kind kind, const wchar_t* gamedir, const wchar_t* mcver,
                   const wchar_t* dir_id, const wchar_t* loader,
                   dl_progress* prog, wchar_t* err, size_t errlen) {
    if (kind == LD_FORGE || kind == LD_NEOFORGE)
        return forge_install(kind, gamedir, mcver, dir_id, loader, prog, err, errlen);

    HINTERNET inet;
    if (!net_init(&inet)) { _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败"); return 0; }

    wchar_t u_bmcl[512], u_off[512];
    const wchar_t* urls[2]; int nu = 0;
    if (kind == LD_QUILT) {
        _snwprintf_s(u_off, 512, _TRUNCATE,
                     L"https://meta.quiltmc.org/v3/versions/loader/%s/%s/profile/json", mcver, loader);
        urls[nu++] = u_off;
    } else {
        _snwprintf_s(u_bmcl, 512, _TRUNCATE,
                     L"https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/%s/%s/profile/json",
                     mcver, loader);
        _snwprintf_s(u_off, 512, _TRUNCATE,
                     L"https://meta.fabricmc.net/v2/versions/loader/%s/%s/profile/json", mcver, loader);
        urls[nu++] = u_bmcl; urls[nu++] = u_off;
    }
    unsigned char* data = NULL; size_t dlen = 0;
    if (!get_first(inet, urls, nu, &data, &dlen)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"下载加载器 profile 失败（该版本可能不存在）");
        InternetCloseHandle(inet); return 0;
    }
    const char* js = (const char*)data;
    jsmntok_t* t = NULL; int nt = 0;
    if (!json_parse_mem(js, dlen, (struct jsmntok**)&t, &nt) || nt < 1) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"加载器 profile 解析失败");
        free(t); free(data); InternetCloseHandle(inet); return 0;
    }
    const jsmntok_t* idt = jm(js, t, "id");
    wchar_t* id = idt ? jwstrdup(js, idt) : NULL;
    if (!id) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"加载器 profile 缺少 id");
        free(t); free(data); InternetCloseHandle(inet); return 0;
    }

    /* 自定义版本：profile 的 id 与 inheritsFrom 都基于原版 mcver，但游戏目录里
     * 只有 dir_id（自定义名）。需把 id 改为含 dir_id、inheritsFrom 改为 dir_id。 */
    const struct jsmntok* inht = jm(js, t, "inheritsFrom");
    int custom = dir_id && !weq(dir_id, mcver);
    if (custom && (!inht || inht->type != JSMN_STRING)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"加载器 profile 缺少 inheritsFrom，无法安装到自定义版本");
        free(t); free(data); free(id); InternetCloseHandle(inet); return 0;
    }
    if (custom) {
        /* 1) 把 id 字符串中的 mcver 替换为 dir_id；2) 把 inheritsFrom 值替换为 dir_id。
         * 直接在保存的 JSON 上做字符串替换：先替换 id，再替换 inheritsFrom。 */
        char* mcverU = w2u(mcver); char* dirU = w2u(dir_id);
        /* 构造新 id：fabric-loader-<loader>-<dir_id>（替换尾部 mcver） */
        /* 用通用替换：在 id token 范围内替换；这里简化为整体替换两处精确子串。 */
        /* 先处理 inheritsFrom（其值恰为 mcver），再处理 id（尾部 mcver）。
         * 为避免误替换 libraries 路径，只在对应 token 范围内操作。 */
        const jsmntok_t* idtok = jm(js, t, "id");
        /* 新 id 文本：把原 id 中的 mcver 子串替换为 dir_id */
        char* oldid = (char*)xalloc(idtok->end - idtok->start + 1);
        memcpy(oldid, js + idtok->start, idtok->end - idtok->start);
        oldid[idtok->end - idtok->start] = 0;
        char* newid = str_replace_all(oldid, mcverU, dirU);
        free(oldid);
        /* 按位置顺序，把 id 与 inheritsFrom 两个 token 的替换合并。
         * 区间：A=id, B=inheritsFrom（顺序不定），排序后拼接。 */
        struct { size_t s, e; char* nv; } R[2];
        R[0].s = idtok->start; R[0].e = idtok->end; R[0].nv = newid;
        R[1].s = inht->start; R[1].e = inht->end; R[1].nv = dirU;
        if (R[1].s < R[0].s) {
            size_t ts; size_t te; char* tn;
            ts = R[0].s; te = R[0].e; tn = R[0].nv;
            R[0].s = R[1].s; R[0].e = R[1].e; R[0].nv = R[1].nv;
            R[1].s = ts; R[1].e = te; R[1].nv = tn;
        }
        size_t total = R[0].s + strlen(R[0].nv) + (R[1].s - R[0].e) +
                       strlen(R[1].nv) + (dlen - R[1].e) + 1;
        char* o2 = (char*)xalloc(total); char* pp = o2;
        memcpy(pp, js, R[0].s); pp += R[0].s;
        memcpy(pp, R[0].nv, strlen(R[0].nv)); pp += strlen(R[0].nv);
        memcpy(pp, js + R[0].e, R[1].s - R[0].e); pp += R[1].s - R[0].e;
        memcpy(pp, R[1].nv, strlen(R[1].nv)); pp += strlen(R[1].nv);
        memcpy(pp, js + R[1].e, dlen - R[1].e); pp += dlen - R[1].e;
        *pp = 0;
        /* 更新 id 为新值（用于目录名） */
        free(id); id = u2w(newid);
        free(data); data = (unsigned char*)o2; dlen = pp - o2;
        js = (const char*)data;
        free(newid); free(mcverU); free(dirU);
        /* 重新解析以收集 libraries */
        free(t);
        if (!json_parse_mem(js, dlen, (struct jsmntok**)&t, &nt) || nt < 1) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"自定义版本 profile 改写失败");
            free(t); free(data); free(id); InternetCloseHandle(inet); return 0;
        }
    }

    wchar_t* vdir = path_join3(gamedir, L"versions", id);
    ensure_dir_rec(vdir);
    wchar_t* jpath = path_join(vdir, wcat3(id, L".json", L""));
    FILE* jf = _wfopen(jpath, L"wb");
    if (jf) { fwrite(data, 1, dlen, jf); fclose(jf); }

    /* 收集加载器自带 libraries（url 为 maven 前缀）；缺失才下载 */
    dl_job* jobs = NULL; size_t njob = 0;
    const jsmntok_t* libs = jm(js, t, "libraries");
    if (libs && libs->type == JSMN_ARRAY) {
        const jsmntok_t* lp = libs + 1;
        for (int i = 0; i < libs->size; i++) {
            const jsmntok_t* l = lp;
            lp = tok_skip(l);
            if (l->type != JSMN_OBJECT) continue;
            const jsmntok_t* ntok = jm(js, l, "name");
            if (!ntok) continue;
            wchar_t* name = jwstrdup(js, ntok);
            const jsmntok_t* ut = jm(js, l, "url");
            wchar_t* base = ut ? jwstrdup(js, ut) : wdup(L"https://libraries.minecraft.net/");
            wchar_t* rel = name_to_rel(name);
            if (!rel) { free(name); free(base); continue; }
            wchar_t* libroot2 = path_join(gamedir, L"libraries");
            wchar_t* fp = path_join(libroot2, rel);
            free(libroot2);
            if (!file_exists(fp)) {
                wsb bu; wsb_init(&bu);
                wsb_add(&bu, base); wsb_add(&bu, rel);
                wchar_t* url = wsb_take(&bu);
                wsb_free(&bu);
                jobs = (dl_job*)xrealloc(jobs, (njob + 1) * sizeof(dl_job));
                jobs[njob].url = url;
                jobs[njob].dest = fp;
                jobs[njob].name = wdup(name);
                jobs[njob].size = 0;
                njob++;
            } else {
                free(fp);
            }
            free(name); free(base); free(rel);
        }
    }
    free(data); free(t);
    InternetCloseHandle(inet);
    int ok = dl_run_jobs(jobs, njob, prog, err, errlen);
    for (size_t i = 0; i < njob; i++) { free(jobs[i].url); free(jobs[i].dest); free(jobs[i].name); }
    free(jobs);
    free(id); free(vdir); free(jpath);
    return ok;
}
