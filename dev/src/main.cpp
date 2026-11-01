/* LLcl - main.c : wWinMain 入口；无参数 → GUI；带参数 → CLI（扫描/Java/自检） */
#include "llcl.h"

/* ---------------- console output for GUI-subsystem exe ---------------- */
static HANDLE g_con = INVALID_HANDLE_VALUE;

static void console_attach(void) {
    if (g_con != INVALID_HANDLE_VALUE) return;
    /* redirected (pipe/file): use inherited std handle first */
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h != NULL && h != INVALID_HANDLE_VALUE) {
        g_con = h;
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
}

void cli_print(const wchar_t* s) {
    console_attach();
    if (g_con == INVALID_HANDLE_VALUE) return;
    DWORD dummy;
    if (WriteConsoleW(g_con, s, (DWORD)wcslen(s), &dummy, NULL)) return;
    /* redirected: fall back to UTF-8 bytes */
    char* u8 = w2u(s);
    if (u8) {
        WriteFile(g_con, u8, (DWORD)strlen(u8), &dummy, NULL);
        free(u8);
    }
}

static void cli_printf(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = _vscwprintf(fmt, ap);
    va_end(ap);
    if (need < 0) return;
    wchar_t* buf = (wchar_t*)xalloc(((size_t)need + 1) * sizeof(wchar_t));
    va_start(ap, fmt);
    _vsnwprintf_s(buf, (size_t)need + 1, (size_t)need, fmt, ap);
    va_end(ap);
    cli_print(buf);
    free(buf);
}

/* ---------------- CLI commands ---------------- */
static void cmd_scan(const wchar_t* gamedir) {
    ventry* vers = NULL;
    int n = 0;
    if (!dir_exists(gamedir)) { cli_printf(L"游戏目录不存在: %s\n", gamedir); return; }
    scan_versions(gamedir, &vers, &n);
    cli_printf(L"扫描 %s → %d 个版本\n", gamedir, n);
    for (int i = 0; i < n; i++) {
        vinfo v;
        if (!resolve_version(gamedir, vers[i].id, &v)) {
            cli_printf(L"  %s: 解析失败\n", vers[i].id);
            continue;
        }
        int nc = 0, np = 0;
        for (size_t k = 0; k < v.nlibs; k++) {
            if (v.libs[k].is_native) nc++;
            if (v.libs[k].present) np++;
        }
        cli_printf(L"  %s: mainClass=%s | libs=%u (native=%u, 存在=%u) | Java>=%d | %s\n",
                   v.id, v.main_class ? v.main_class : L"-", (unsigned)v.nlibs, (unsigned)nc, (unsigned)np,
                   v.java_major, v.has_modern ? L"arguments" : (v.has_legacy ? L"minecraftArguments" : L"无参数"));
        vinfo_free(&v);
    }
    for (int i = 0; i < n; i++) { free(vers[i].id); free(vers[i].json_path); }
    free(vers);
}

static void cmd_detectjava(void) {
    javaitem* list = NULL;
    int n = 0;
    detect_javas(&list, &n);
    cli_printf(L"检测到 %d 个 Java\n", n);
    for (int i = 0; i < n; i++)
        cli_printf(L"  Java %d (%s) %d位: %s\n", list[i].major, list[i].fullver, list[i].is64 ? 64 : 32, list[i].path);
    if (n == 0) cli_printf(L"  未检测到 Java。老机器常见：请安装 Java 8（旧版本）/17/21。\n");
    javas_free(list, n);
}

/* 测试用：--javadl <major> [32/64] 自动下载并安装 Java */
static void cmd_javadl(int major, int is64) {
    cli_printf(L"开始下载 Java %d（%d 位）…\n", major, is64 ? 64 : 32);
    dl_progress prog;
    memset(&prog, 0, sizeof(prog));
    wchar_t err[256] = {0};
    int ok = javadl_install(major, is64, &prog, err, 256);
    if (ok) {
        wchar_t* j = javadl_find(major, is64);
        cli_printf(L"Java %d 安装成功: %s\n", major, j ? j : L"?");
        free(j);
    } else {
        cli_printf(L"Java 下载失败: %s\n", err[0] ? err : L"未知错误");
    }
}

/* 测试用：--modpack <mrpack> <游戏目录> */
static void cmd_modpack(const wchar_t* mrpack, const wchar_t* gamedir) {
    if (!file_exists(mrpack)) { cli_printf(L"找不到整合包文件: %s\n", mrpack); return; }
    cli_printf(L"开始安装整合包 %s -> %s\n", mrpack, gamedir);
    dl_progress prog;
    memset(&prog, 0, sizeof(prog));
    wchar_t err[256] = {0};
    int ok = modpack_install(gamedir, mrpack, 0, &prog, err, 256);
    if (ok) cli_printf(L"整合包安装成功！\n");
    else cli_printf(L"整合包安装失败: %s\n", err[0] ? err : L"未知错误");
}

static void cmd_uuid(const wchar_t* name) {
    wchar_t u[64];
    uuid_offline(name, u, 64);
    cli_printf(L"OfflinePlayer:%s -> %s\n", name, u);
}

static void cmd_mem(void) {
    uint64_t phys = total_phys_bytes();
    wchar_t* s = fmt_bytes(phys);
    cli_printf(L"物理内存: %s | 自动堆: %dMB | Xms: %dMB\n", s, auto_heap_mb(), auto_xms_mb(auto_heap_mb()));
    free(s);
}

/* 测试用：--launch <游戏目录> <版本> <java.exe> <内存MB> <玩家名> [隔离0/1]（不等待子进程） */
static void cmd_launch(const wchar_t* gamedir, const wchar_t* ver, const wchar_t* java, int mem, const wchar_t* player, int isolate) {
    vinfo v;
    if (!resolve_version(gamedir, ver, &v)) { cli_printf(L"版本解析失败: %s\n", ver); return; }
    lresult r;
    int ok = launch_game(&v, gamedir, java, mem, player, NULL, NULL, NULL, isolate, 1, &r);
    cli_printf(L"launch=%d pid=%lu missing_libs=%d\n", ok, (unsigned long)r.pid, r.missing_libs);
    if (r.err[0]) cli_printf(L"错误: %s\n", r.err);
    cli_printf(L"耗时: natives=%u ms build=%u ms spawn=%u ms total=%u ms\n",
               (unsigned)(r.us_natives / 1000), (unsigned)(r.us_build / 1000),
               (unsigned)(r.us_spawn / 1000), (unsigned)(r.us_total / 1000));
    vinfo_free(&v);
}

/* 测试用：--download <游戏目录> <版本> [assets 0/1] [自定义版本名]
   （下载版本 JSON/client/libraries/assets；自定义名非空时安装到 versions/<自定义名>/） */
static void cmd_download(const wchar_t* gamedir, const wchar_t* ver, int skip_assets, const wchar_t* custom) {
    dl_progress prog;
    memset(&prog, 0, sizeof(prog));
    wchar_t err[640] = L"";
    cli_printf(L"开始下载 %s（assets %s%s）...\n", ver, skip_assets ? L"跳过" : L"全量",
               (custom && custom[0]) ? wcat3(L"，自定义名 ", custom, L"") : L"");
    int ok = dl_install_version(gamedir, ver, custom, skip_assets, &prog, err, 640);
    if (err[0]) cli_printf(L"提示: %s\n", err);
    if (ok) cli_printf(L"下载完成: %s（文件 %d/%d）\n",
                       (custom && custom[0]) ? custom : ver, prog.files_done, prog.files_total);
    else cli_printf(L"下载失败: %s\n", err[0] ? err : L"未知错误");
}

/* 测试用：--loader <fabric/quilt/forge/neoforge> <游戏目录> <mc版本> [loader版本]
   不指定 loader 版本时，自动选第一个稳定版安装。 */
static void cmd_loader(const wchar_t* kind, const wchar_t* gamedir, const wchar_t* mcver, const wchar_t* loader) {
    ld_kind k = LD_FABRIC;
    if (wcscmp(kind, L"quilt") == 0) k = LD_QUILT;
    else if (wcscmp(kind, L"forge") == 0) k = LD_FORGE;
    else if (wcscmp(kind, L"neoforge") == 0) k = LD_NEOFORGE;
    wchar_t err[640] = L"";
    wchar_t* lv = NULL;
    if (!loader) {
        ld_entry* arr = NULL; int n = 0;
        if (!loader_list(k, mcver, &arr, &n, err, 640)) {
            cli_printf(L"获取加载器列表失败: %s\n", err); return;
        }
        for (int i = 0; i < n; i++) {
            if (arr[i].stable) { lv = wdup(arr[i].version); break; }
        }
        if (!lv) lv = wdup(arr[0].version);
        cli_printf(L"自动选择加载器版本: %s（共 %d 个）\n", lv, n);
        loader_list_free(arr, n);
    } else {
        lv = wdup(loader);
    }
    dl_progress prog;
    memset(&prog, 0, sizeof(prog));
    cli_printf(L"开始安装 %s %s for %s ...\n", loader_kind_name(k), lv, mcver);
    /* CLI 直接传原版版本，dir_id = mcver */
    int ok = loader_install(k, gamedir, mcver, mcver, lv, &prog, err, 640);
    if (err[0]) cli_printf(L"提示: %s\n", err);
    if (ok) cli_printf(L"安装完成（库文件 %d/%d）\n", prog.files_done, prog.files_total);
    else cli_printf(L"安装失败: %s\n", err[0] ? err : L"未知错误");
    free(lv);
}

static void cmd_msadevice(void) {
    wchar_t code[64] = L"", dcode[4096] = L"", vurl[256] = L"", err[640] = L"";
    int interval = 0, expires_in = 0;
    if (msa_devicecode(NULL, code, 64, dcode, 4096, vurl, 256, err, 640, &interval, &expires_in)) {
        cli_printf(L"devicecode=OK user_code=%s verify=%s dcode_len=%d interval=%d expires=%d\n",
                   code, vurl, (int)wcslen(dcode), interval, expires_in);
    } else {
        cli_printf(L"devicecode=FAIL err=%s\n", err);
    }
}

/* base64url 解码（仅诊断用，用于查看 JWT 的 claims） */
static int b64url_decode(const char* s, size_t n, unsigned char* out, size_t outsz) {
    static signed char tab[256];
    static int tab_ready = 0;
    if (!tab_ready) {
        memset(tab, -1, sizeof(tab));
        const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; alpha[i]; i++) tab[(unsigned char)alpha[i]] = (signed char)i;
        tab_ready = 1;
    }
    int val = 0, bits = 0; size_t oi = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '=' || c == '\r' || c == '\n') break;
        signed char d = tab[c];
        if (d < 0) continue;
        val = (val << 6) | d; bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (oi >= outsz) return -1;
            out[oi++] = (unsigned char)((val >> bits) & 0xFF);
        }
    }
    if (oi < outsz) out[oi] = 0;
    return (int)oi;
}

static void print_jwt_claims(const wchar_t* token, const wchar_t* tag) {
    char* t = w2u(token);
    if (!t) return;
    char* p1 = strchr(t, '.');
    if (!p1) { cli_printf(L"[%s] 该 token 不是 JWT 格式（MSA 可能返回了不透明 token）\n", tag); free(t); return; }
    char* p2 = strchr(p1 + 1, '.');
    size_t plen = p2 ? (size_t)(p2 - p1 - 1) : strlen(p1 + 1);
    if (plen > 3000) plen = 3000;
    unsigned char claims[3001];
    int dl = b64url_decode(p1 + 1, plen, claims, sizeof(claims) - 1);
    if (dl > 0) {
        claims[dl] = 0;
        wchar_t* w = u2w((char*)claims);
        cli_printf(L"[%s] JWT claims: %s\n", tag, w ? w : L"(解码失败)");
        free(w);
    } else {
        cli_printf(L"[%s] JWT claims 解码失败\n", tag);
    }
    free(t);
}

/* 完整设备码登录诊断：人工授权一次，打印每一步真实状态，用于定位 XBL/XSTS 问题 */
static void cmd_msalogin(void) {
    wchar_t err[640] = L"";
    wchar_t code[64] = L"", dcode[4096] = L"", vurl[256] = L"";
    wchar_t access[8192] = L"", refresh[8192] = L"";
    int interval = 5, expires_in = 900;
    if (!msa_devicecode(NULL, code, 64, dcode, 4096, vurl, 256, err, 640, &interval, &expires_in)) {
        cli_printf(L"[1] devicecode=FAIL %s\n", err);
        return;
    }
    cli_printf(L"[1] 请在浏览器打开: %s\n    输入代码: %s\n    （有效期 %d 秒，轮询间隔 %d 秒）\n",
               vurl, code, expires_in, interval);
    cli_printf(L"[2] 等待授权，请在浏览器完成登录...\n");
    if (!msa_poll_token(NULL, dcode, interval, access, 8192, refresh, 8192, err, 640)) {
        cli_printf(L"[2] token=FAIL %s\n", err);
        return;
    }
    cli_printf(L"[2] token=OK access_len=%d refresh_len=%d\n", (int)wcslen(access), (int)wcslen(refresh));
    {
        char* ap = w2u(access);
        if (ap) {
            char head[40] = "";
            int dots = 0;
            for (char* q = ap; *q; q++) if (*q == '.') dots++;
            strncpy_s(head, sizeof(head), ap, _TRUNCATE);
            cli_printf(L"[2b] access_head=%S dots=%d\n", head, dots);
            free(ap);
        }
    }
    print_jwt_claims(access, L"2a");
    /* 路径 A：直接用设备码返回的 access_token 走 Xbox 链路 */
    {
        wchar_t uuid[128] = L"", name[128] = L"", xuid[64] = L"", e2[640] = L"";
        if (msa_token_login(access, uuid, 128, name, 128, xuid, 64, e2, 640)) {
            cli_printf(L"[3] direct-access=OK name=%s uuid=%s xuid=%s\n", name, uuid, xuid);
        } else {
            cli_printf(L"[3] direct-access=FAIL %s\n", e2);
            /* 路径 B：用 refresh_token 换一个 access_token 再试 */
            wchar_t uuid2[128] = L"", name2[128] = L"", xuid2[64] = L"", rn[8192] = L"", e3[640] = L"";
            wchar_t na[8192] = L"";
            if (msa_restore(NULL, refresh, uuid2, 128, name2, 128, xuid2, 64, na, 8192, rn, 8192, e3, 640))
                cli_printf(L"[4] refresh-path=OK name=%s uuid=%s xuid=%s\n", name2, uuid2, xuid2);
            else
                cli_printf(L"[4] refresh-path=FAIL %s\n", e3);
        }
    }
}

static void cmd_msatoken(const wchar_t* tok) {
    wchar_t uuid[128] = L"", name[128] = L"", xuid[64] = L"", err[640] = L"";
    if (msa_token_login(tok, uuid, 128, name, 128, xuid, 64, err, 640)) {
        cli_printf(L"token_login=OK name=%s uuid=%s xuid=%s\n", name, uuid, xuid);
    } else {
        cli_printf(L"token_login=FAIL err=%s\n", err);
    }
}

static void cmd_srvtest(const wchar_t* dir) {
    wchar_t* path = path_join(dir, L"servers.dat");
    srv_entry* e = NULL;
    int n = 0;
    int ok = servers_load_path(path, &e, &n);
    cli_printf(L"servers.dat: load=%d n=%d path=%s\n", ok, n, path);
    for (int i = 0; i < n; i++)
        cli_printf(L"  [%d] name=%s ip=%s hidden=%d\n", i,
                   e[i].name ? e[i].name : L"-", e[i].ip ? e[i].ip : L"-", e[i].hidden);
    server_list_free(e, n);
    free(path);
}

static void cmd_cfgread(const wchar_t* dir) {
    cfg c;
    cfg_init(&c);
    wchar_t* ini = path_join(dir, L"LLcl.ini");
    int ok = cfg_load(ini, &c);
    cli_printf(L"cfg_load=%d path=%s\n", ok, ini);
    cli_printf(L"  player=%s\n", c.player ? c.player : L"(null)");
    cli_printf(L"  game_dir=%s\n", c.game_dir ? c.game_dir : L"(null)");
    cli_printf(L"  java=%s\n", c.java_path ? c.java_path : L"(null)");
    cli_printf(L"  mem=%d isolate=%d\n", c.mem_mb, c.version_isolate);
    cfg_free(&c);
    free(ini);
}

static void cmd_configtest(const wchar_t* dir) {
    wchar_t* path = path_join(dir, L"cfgtest.ini");
    cfg c;
    cfg_init(&c);
    c.java_path = wdup(L"C:\\Program Files\\Java\\jdk-17\\bin\\java.exe");
    c.mem_mb = 2048;
    c.player = wdup(L"测试玩家");
    c.game_dir = wdup(L"D:\\测试 目录\\.minecraft");
    c.last_version = wdup(L"1.20.1");
    int ok1 = cfg_save(path, &c);
    cfg d;
    cfg_init(&d);
    int ok2 = cfg_load(path, &d);
    /* 注意：比较须在 cfg_free(&c) 之前完成 */
    int pass = ok1 && ok2 && d.mem_mb == 2048 &&
               weq(d.java_path, L"C:\\Program Files\\Java\\jdk-17\\bin\\java.exe") &&
               weq(d.player, L"测试玩家") &&
               weq(d.game_dir, L"D:\\测试 目录\\.minecraft") &&
               weq(d.last_version, L"1.20.1");
    cfg_free(&c);
    cli_printf(L"config roundtrip: save=%d load=%d %s\n", ok1, ok2, pass ? L"PASS" : L"FAIL");
    if (ok2) {
        cli_printf(L"  读回: java=%s | mem=%d | player=%s | dir=%s | ver=%s\n",
                   d.java_path, d.mem_mb, d.player, d.game_dir, d.last_version);
    }
    cfg_free(&d);
    DeleteFileW(path);
    free(path);
}

/* ---------------- selftest ---------------- */
static int g_fails = 0;
static void check(int ok, const wchar_t* name, const wchar_t* detail) {
    cli_printf(L"  [%s] %s%s%s\n", ok ? L"PASS" : L"FAIL", name, detail && *detail ? L": " : L"", detail ? detail : L"");
    if (!ok) g_fails++;
}

static void selftest(const wchar_t* gamedir) {
    cli_printf(L"== LLcl selftest ==\n");
    /* 1. scan */
    ventry* vers = NULL;
    int n = 0;
    int s1 = scan_versions(gamedir, &vers, &n);
    int has189 = 0, has1201 = 0, haspar = 0, hasch = 0;
    for (int i = 0; i < n; i++) {
        if (weq(vers[i].id, L"1.8.9")) has189 = 1;
        if (weq(vers[i].id, L"1.20.1")) has1201 = 1;
        if (weq(vers[i].id, L"_test_parent")) haspar = 1;
        if (weq(vers[i].id, L"_test_child")) hasch = 1;
    }
    wchar_t d1[128];
    _snwprintf_s(d1, 128, _TRUNCATE, L"%d 个版本", n);
    check(s1 && has189 && has1201 && haspar && hasch, L"scan_versions", d1);

    /* 2. resolve 1.8.9 (legacy args) */
    {
        vinfo v;
        int ok = resolve_version(gamedir, L"1.8.9", &v);
        check(ok && v.has_legacy && !v.has_modern && v.main_class &&
              weq(v.main_class, L"net.minecraft.client.main.Main") && v.nlibs >= 30,
              L"resolve 1.8.9 (legacy)", L"");
        if (ok) {
            cli_printf(L"     1.8.9: libs=%u main=%s\n", (unsigned)v.nlibs, v.main_class);
            vinfo_free(&v);
        }
    }
    /* 3. resolve 1.20.1 (modern args, java 17) */
    {
        vinfo v;
        int ok = resolve_version(gamedir, L"1.20.1", &v);
        check(ok && v.has_modern && !v.has_legacy && v.java_major == 17 && v.nlibs >= 60,
              L"resolve 1.20.1 (arguments, Java17)", L"");
        if (ok) {
            int nc2 = 0;
            for (size_t k = 0; k < v.nlibs; k++) if (v.libs[k].is_native) nc2++;
            cli_printf(L"     1.20.1: libs=%u (native=%u) jvm=%u game=%u\n", (unsigned)v.nlibs, (unsigned)nc2,
                       (unsigned)v.jvm.n, (unsigned)v.game.n);
            vinfo_free(&v);
        }
    }
    /* 3b. 26.3 (Java 25, 新 natives 布局) —— 测试目录存在时才检查 */
    {
        int has263 = 0;
        for (int i = 0; i < n; i++) if (weq(vers[i].id, L"26.3")) has263 = 1;
        if (has263) {
            vinfo v;
            int ok = resolve_version(gamedir, L"26.3", &v);
            int nat = 0;
            for (size_t k = 0; ok && k < v.nlibs; k++) if (v.libs[k].is_native) nat++;
            check(ok && v.has_modern && v.java_major == 25 && weq(v.assets, L"34") && nat >= 10,
                  L"resolve 26.3 (Java25 / assets34 / natives)", L"");
            if (ok) {
                cli_printf(L"     26.3: libs=%u native=%u java=%d assets=%s\n",
                           (unsigned)v.nlibs, (unsigned)nat, v.java_major,
                           v.assets ? v.assets : L"-");
                vinfo_free(&v);
            }
        }
    }
    /* 3c. JSON UTF-8 / \uXXXX 解码不能出现双重编码 */
    {
        const char* j = "{\"name\":\"\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95\",\"esc\":\"\\u4E2D\\u6587\"}";
        struct jsmntok* tk = NULL;
        int nt = 0;
        int ok = json_parse_mem(j, strlen(j), &tk, &nt) && nt > 0;
        char* a = ok ? jstrdup(j, jm(j, (const struct jsmntok*)tk, "name")) : NULL;
        char* b = ok ? jstrdup(j, jm(j, (const struct jsmntok*)tk, "esc")) : NULL;
        wchar_t* wa = a ? u2w(a) : NULL;
        wchar_t* wb = b ? u2w(b) : NULL;
        check(wa && wb && weq(wa, L"中文测试") && weq(wb, L"中文"), L"json utf-8 / \\u decode", L"");
        free(a); free(b); free(wa); free(wb); free(tk);
    }
    /* 4. inheritsFrom merge */
    {
        vinfo v;
        int ok = resolve_version(gamedir, L"_test_child", &v);
        int nc = 0, dup = 0;
        for (size_t k = 0; k < v.nlibs; k++) if (v.libs[k].is_native) nc++;
        if (v.nlibs >= 2) for (size_t a = 0; a < v.nlibs; a++) for (size_t b = a + 1; b < v.nlibs; b++)
            if (weqi(v.libs[a].name, v.libs[b].name)) dup = 1;
        check(ok && v.main_class && weq(v.main_class, L"net.minecraft.launchwrapper.Launch") &&
              v.nlibs == 3 && nc == 1 && !dup && v.has_legacy && v.java_major == 8,
              L"resolve _test_child (inheritsFrom merge)", L"");
        if (ok) {
            cli_printf(L"     child: main=%s libs=%u native=%u javaMajor=%d legacy=%d\n",
                       v.main_class, (unsigned)v.nlibs, (unsigned)nc, v.java_major, v.has_legacy);
            vinfo_free(&v);
        }
    }
    /* 5. uuid */
    {
        wchar_t u[64];
        uuid_offline(L"Steve", u, 64);
        check(weq(u, L"5627dd98-e6be-3c21-b8a8-e92344183641"), L"uuid Steve", u);
        uuid_offline(L"Notch", u, 64);
        check(weq(u, L"b50ad385-829d-3141-a216-7e7d7539ba7f"), L"uuid Notch", u);
        uuid_offline(L"张三", u, 64);
        check(weq(u, L"17eb791e-5c32-3d5f-9c7d-ec2e5e6fa3d6"), L"uuid 张三 (utf8)", u);
    }
    /* 6. md5 vector */
    {
        unsigned char d[16];
        md5_of((const unsigned char*)"abc", 3, d);
        unsigned char expect[16] = { 0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72 };
        check(memcmp(d, expect, 16) == 0, L"md5(\"abc\")", L"");
    }
    /* 7. config roundtrip */
    cmd_configtest(gamedir);
    /* 8. java detect (informational) */
    {
        javaitem* list = NULL;
        int jn = 0;
        detect_javas(&list, &jn);
        cli_printf(L"   [INFO] 检测到 %d 个 Java:\n", jn);
        for (int i = 0; i < jn; i++)
            cli_printf(L"          Java %d (%s) %d位: %s\n", list[i].major, list[i].fullver,
                       list[i].is64 ? 64 : 32, list[i].path);
        check(jn >= 1, L"detect_javas (本机应有 Java)", L"");
        javas_free(list, jn);
    }
    /* 9. memory tier (informational) */
    {
        uint64_t phys = total_phys_bytes();
        wchar_t* s = fmt_bytes(phys);
        cli_printf(L"   [INFO] 物理内存 %s -> 自动堆 %dMB (Xms %dMB)\n", s, auto_heap_mb(), auto_xms_mb(auto_heap_mb()));
        check(auto_heap_mb() >= 384, L"auto_heap_mb tier", L"");
        free(s);
    }
    /* 10. command line build smoke test: build via launch on a fake java that prints args */
    {
        /* use detected java to print its own -version; full launch path needs real jars — skip */
        cli_printf(L"   [INFO] 命令行拼接/argfile 由实测环节覆盖（见 README 与 e2e 测试脚本）\n");
    }
    for (int i = 0; i < n; i++) { free(vers[i].id); free(vers[i].json_path); }
    free(vers);
    cli_printf(L"== selftest %s: %d 项失败 ==\n", g_fails == 0 ? L"全部通过" : L"存在失败", g_fails);
}

static void cmd_manifest(void) {
    dl_list l;
    memset(&l, 0, sizeof(l));
    wchar_t err[640] = L"";
    int ok = dl_fetch_manifest(&l, err, 640);
    cli_printf(L"manifest=%d count=%d err=%s\n", ok, l.n, err);
    for (int i = 0; i < l.n && i < 8; i++)
        cli_printf(L"  [%d] %s -> %s\n", i, l.v[i].id, l.v[i].url ? l.v[i].url : L"(null)");
    for (int i = 0; i < l.n; i++) if (weq(l.v[i].id, L"26.3")) { cli_printf(L"  FOUND 26.3: %s\n", l.v[i].url); break; }
    dl_list_free(&l);
}

static void cmd_help(void) {
    cli_print(L"LLcl - Minecraft Java 启动器 (v" LLCL_VERSION L")\n"
              L"用法:\n"
              L"  LLcl.exe                      启动图形界面\n"
              L"  LLcl.exe --help               本帮助\n"
              L"  LLcl.exe --scan <游戏目录>     扫描并解析版本（含 inheritsFrom 合并）\n"
              L"  LLcl.exe --detectjava         探测本机 Java\n"
              L"  LLcl.exe --manifest           拉取版本清单（诊断下载源）\n"
              L"  LLcl.exe --uuid <玩家名>       计算离线 UUID\n"
              L"  LLcl.exe --mem                显示自动 JVM 调参结果\n"
              L"  LLcl.exe --configtest <目录>   配置文件读写往返测试\n"
              L"  LLcl.exe --download <游戏目录> <版本> [assets 0/1] [自定义版本名]  下载游戏\n"
              L"  LLcl.exe --msa-device       申请设备码（调试）\n"
              L"  LLcl.exe --msa-login        完整设备码登录诊断（按提示在浏览器授权）\n"
              L"  LLcl.exe --msa-token <tok>  用 access_token 直接登录（调试）\n"
              L"  LLcl.exe --relay [端口]       启动远程联机中转服务器（默认 25566）\n"
              L"  LLcl.exe --net-host direct|relay <中转地址|-> <MC端口> <秒>  命令行开房\n"
              L"  LLcl.exe --net-join <邀请码> [秒]  命令行加入房间\n"
              L"  LLcl.exe --nettest            联机回环自检（中转/直连/代理/转发）\n"
              L"  LLcl.exe --selftest <游戏目录>  综合自检（扫描/合并/UUID/MD5/配置/Java/内存）\n");
}

int WINAPI wWinMain(HINSTANCE h, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    (void)hPrev; (void)nShow;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(lpCmd, &argc);
    /* 仅当确实传入了命令行参数时才走 CLI；双击/GUI 启动时 lpCmd 为空 → argv[0] 不是 "-" 开头 */
    if (argc >= 1 && argv && argv[0] && argv[0][0] &&
        (argv[0][0] == L'-' || argv[0][0] == L'/')) {
        if (weq(argv[0], L"--help") || weq(argv[0], L"-h") || weq(argv[0], L"/?")) {
            cmd_help();
            LocalFree(argv);
            return 0;
        }
        if (weq(argv[0], L"--scan") && argc > 1) { cmd_scan(argv[1]); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--detectjava")) { cmd_detectjava(); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--manifest")) { cmd_manifest(); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--javadl") && argc > 1) {
            int is64 = (argc > 2 && (weq(argv[2], L"32") || weq(argv[2], L"x86"))) ? 0 : 1;
            cmd_javadl(_wtoi(argv[1]), is64); LocalFree(argv); return 0;
        }
        if (weq(argv[0], L"--modpack") && argc > 2) {
            cmd_modpack(argv[1], argv[2]); LocalFree(argv); return 0;
        }
        if (weq(argv[0], L"--uuid") && argc > 1) { cmd_uuid(argv[1]); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--mem")) { cmd_mem(); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--launch") && argc > 5) {
            cmd_launch(argv[1], argv[2], argv[3], _wtoi(argv[4]), argv[5],
                       argc > 6 ? _wtoi(argv[6]) : 0);
            LocalFree(argv);
            return 0;
        }
        if (weq(argv[0], L"--configtest") && argc > 1) { cmd_configtest(argv[1]); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--srvtest") && argc > 1) { cmd_srvtest(argv[1]); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--cfgread") && argc > 1) { cmd_cfgread(argv[1]); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--download") && argc > 2) {
            int skip = argc > 3 ? (_wtoi(argv[3]) == 0 ? 0 : 1) : 0;
            const wchar_t* cust = argc > 4 ? argv[4] : NULL;
            cmd_download(argv[1], argv[2], skip, cust);
            LocalFree(argv); return 0;
        }
        if (weq(argv[0], L"--loader") && argc > 3) {
            cmd_loader(argv[1], argv[2], argv[3], argc > 4 ? argv[4] : NULL);
            LocalFree(argv); return 0;
        }
        if (weq(argv[0], L"--msa-device")) { cmd_msadevice(); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--msa-login")) { cmd_msalogin(); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--msa-token") && argc > 1) { cmd_msatoken(argv[1]); LocalFree(argv); return 0; }
        if (weq(argv[0], L"--selftest") && argc > 1) {
            selftest(argv[1]);
            LocalFree(argv);
            return g_fails == 0 ? 0 : 1;
        }
        if (weq(argv[0], L"--relay")) {
            int port = argc > 1 ? _wtoi(argv[1]) : 25566;
            int rc = net_relay_run(port);
            LocalFree(argv);
            return rc;
        }
        if (weq(argv[0], L"--nettest")) {
            int rc = net_selftest();
            LocalFree(argv);
            return rc;
        }
        if (weq(argv[0], L"--net-host") && argc > 4) {
            int mode = weq(argv[1], L"relay") ? NET_MODE_RELAY : NET_MODE_DIRECT;
            int rc = net_cli_host(mode, weq(argv[2], L"-") ? NULL : argv[2],
                                  _wtoi(argv[3]), _wtoi(argv[4]));
            LocalFree(argv);
            return rc;
        }
        if (weq(argv[0], L"--net-join") && argc > 1) {
            int rc = net_cli_join(argv[1], argc > 2 ? _wtoi(argv[2]) : 30);
            LocalFree(argv);
            return rc;
        }
        cli_printf(L"未知参数: %s（用 --help 查看用法）\n", argv[0]);
        LocalFree(argv);
        return 2;
    }
    if (argv) LocalFree(argv);
    (void)h;
    ui_init(h);
    return ui_run();
}
