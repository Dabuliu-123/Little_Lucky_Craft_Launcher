/* LLcl - java.c : Java 探测（JAVA_HOME / 注册表 / 常见路径 / PATH），
 * java -version 验证，PE 头位数检测 */
#include "llcl.h"

/* ---------------- run child & capture output ---------------- */
/* Drain everything currently available from a pipe without blocking.
 * If the caller buffer is full, remaining bytes are discarded (not blocked). */
static void drain_pipe(HANDLE h, char* out, size_t cap, size_t* used) {
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
        DWORD want = avail > 4096 ? 4096 : avail;
        char tmp[4096];
        DWORD rd = 0;
        if (!ReadFile(h, tmp, want, &rd, NULL) || rd == 0) break;
        if (*used + 1 < cap) {
            size_t take = rd;
            if (take > cap - 1 - *used) take = cap - 1 - *used;
            memcpy(out + *used, tmp, take);
            *used += take;
        }
    }
}

int run_capture(const wchar_t* exe, const wchar_t* argline, char* out, size_t cap, int timeout_ms) {
    if (!out || cap == 0) return 0;
    out[0] = 0;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;
    HANDLE r1 = NULL, w1 = NULL, r2 = NULL, w2 = NULL;
    if (!CreatePipe(&r1, &w1, &sa, 0) || !CreatePipe(&r2, &w2, &sa, 0)) {
        if (r1) CloseHandle(r1);
        if (w1) CloseHandle(w1);
        if (r2) CloseHandle(r2);
        if (w2) CloseHandle(w2);
        return 0;
    }
    SetHandleInformation(r1, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(r2, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = w1;
    si.hStdError = w2;

    wchar_t* cmd = wcat3(L"\"", exe, L"\" ");
    wchar_t* full = wcat3(cmd, argline, L"");
    free(cmd);
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(exe, full, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(full);
        CloseHandle(r1); CloseHandle(w1); CloseHandle(r2); CloseHandle(w2);
        return 0;
    }
    free(full);
    CloseHandle(w1);
    CloseHandle(w2);

    size_t used = 0;
    DWORD tmo = (DWORD)(timeout_ms > 0 ? timeout_ms : 5000);
    DWORD start = GetTickCount();
    for (;;) {
        drain_pipe(r1, out, cap, &used);
        drain_pipe(r2, out, cap, &used);
        if (WaitForSingleObject(pi.hProcess, 40) == WAIT_OBJECT_0) {
            /* 进程结束后管道里可能还剩数据：再排空一次 */
            drain_pipe(r1, out, cap, &used);
            drain_pipe(r2, out, cap, &used);
            break;
        }
        if (GetTickCount() - start > tmo) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            drain_pipe(r1, out, cap, &used);
            drain_pipe(r2, out, cap, &used);
            break;
        }
    }
    out[used < cap ? used : cap - 1] = 0;
    CloseHandle(r1); CloseHandle(r2);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* ---------------- version parse ---------------- */
static int parse_java_version(const char* txt, wchar_t* fullver, size_t cap, int* major) {
    /* look for "version \"..." or 'version 1.8.0_xxx' */
    const char* p = txt;
    const char* q = NULL;
    while ((q = strstr(p, "version")) != NULL) {
        const char* r = q + 7;
        while (*r == ' ' || *r == '"' || *r == '=') r++;
        if (*r >= '0' && *r <= '9') {
            const char* e = r;
            while (*e && *e != '"' && *e != '\r' && *e != '\n') e++;
            /* bounded fragment copy（原始缓冲非 NUL 结尾） */
            char frag[128];
            size_t el = (size_t)(e - r);
            if (el >= sizeof(frag)) el = sizeof(frag) - 1;
            memcpy(frag, r, el);
            frag[el] = 0;
            wchar_t tmp[128];
            _snwprintf_s(tmp, 128, _TRUNCATE, L"%hs", frag);
            size_t tl = wcslen(tmp);
            while (tl > 0 && (tmp[tl - 1] == '"' || tmp[tl - 1] == ' ')) tmp[--tl] = 0;
            wcscpy_s(fullver, cap, tmp);
            /* major: "1.x" -> x, else first number */
            int first = atoi(r);
            if (first == 1) {
                const char* d = strchr(r, '.');
                *major = d ? atoi(d + 1) : 1;
            } else {
                *major = first;
            }
            return 1;
        }
        p = q + 7;
        if (p - txt > 65536) break;
    }
    return 0;
}

/* ---------------- arch from PE header ---------------- */
static int pe_is64(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 1;
    unsigned char buf[4096];
    DWORD rd = 0;
    BOOL okr = ReadFile(h, buf, sizeof(buf), &rd, NULL);
    CloseHandle(h);
    if (!okr || rd < 0x40) return 1;
    unsigned int e_lfanew = (unsigned)buf[0x3C] | ((unsigned)buf[0x3D] << 8) | ((unsigned)buf[0x3E] << 16) | ((unsigned)buf[0x3F] << 24);
    if (e_lfanew + 6 > rd) return 1;
    unsigned short machine = (unsigned short)(buf[e_lfanew + 4] | (buf[e_lfanew + 5] << 8));
    return machine != 0x014C; /* IMAGE_FILE_MACHINE_I386 = 0x14c; anything else treated 64-bit */
}

/* ---------------- candidates ---------------- */
static void cand_add(wsa* a, const wchar_t* java_exe) {
    if (!java_exe || !file_exists(java_exe)) return;
    if (!wsa_has(a, java_exe)) wsa_add(a, java_exe);
}

static void add_path_java(wsa* a, const wchar_t* dir) {
    wchar_t* j = path_join(dir, L"bin\\java.exe");
    cand_add(a, j);
    free(j);
}

static void glob_dirs(wsa* dirs, const wchar_t* parent, const wchar_t* pattern) {
    wchar_t pat[4096];
    _snwprintf_s(pat, 4096, _TRUNCATE, L"%s\\%s", parent, pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t* d = path_join(parent, fd.cFileName);
            wsa_add(dirs, d);
            free(d);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void add_registry_javas(wsa* a, const wchar_t* root) {
    HKEY base;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ, &base) != ERROR_SUCCESS) return;
    wchar_t sub1[256];
    DWORD i = 0;
    DWORD len1 = 256;
    while (RegEnumKeyExW(base, i++, sub1, &len1, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        len1 = 256;
        HKEY k1;
        wchar_t kpath[512];
        _snwprintf_s(kpath, 512, _TRUNCATE, L"%s\\%s", root, sub1);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kpath, 0, KEY_READ, &k1) == ERROR_SUCCESS) {
            /* JavaHome directly on this key (e.g. JavaSoft\JDK\current\JavaHome) */
            wchar_t home[1024];
            DWORD sz = sizeof(home);
            if (RegQueryValueExW(k1, L"JavaHome", NULL, NULL, (LPBYTE)home, &sz) == ERROR_SUCCESS && sz > 0) {
                home[sz / sizeof(wchar_t) - 1] = 0;
                add_path_java(a, home);
            }
            /* version subkeys: JavaSoft\<vendor>\<version>\JavaHome */
            wchar_t sub2[256];
            DWORD j = 0, len2 = 256;
            while (RegEnumKeyExW(k1, j++, sub2, &len2, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
                len2 = 256;
                wchar_t subpath[1024];
                _snwprintf_s(subpath, 1024, _TRUNCATE, L"%s\\%s", kpath, sub2);
                HKEY k3;
                if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subpath, 0, KEY_READ, &k3) == ERROR_SUCCESS) {
                    wchar_t home3[1024];
                    DWORD sz3 = sizeof(home3);
                    if (RegQueryValueExW(k3, L"JavaHome", NULL, NULL, (LPBYTE)home3, &sz3) == ERROR_SUCCESS && sz3 > 0) {
                        home3[sz3 / sizeof(wchar_t) - 1] = 0;
                        add_path_java(a, home3);
                    }
                    RegCloseKey(k3);
                }
            }
            RegCloseKey(k1);
        }
    }
    RegCloseKey(base);
}

static void glob_java_from(wsa* a, const wchar_t* parent, const wchar_t* pattern) {
    wsa ds;
    memset(&ds, 0, sizeof(ds));
    glob_dirs(&ds, parent, pattern);
    for (size_t i = 0; i < ds.n; i++) add_path_java(a, ds.v[i]);
    wsa_free(&ds);
}

/* 便携版 runtime：exe 目录\runtime，以及 LLCL_RUNTIME_DIR 环境变量（开发复用） */
static void add_runtime_javas(wsa* a) {
    wchar_t* ed = get_exe_dir();
    wchar_t* rt = path_join(ed, L"runtime");
    free(ed);
    glob_java_from(a, rt, L"*");
    free(rt);
    wchar_t env[4096];
    if (GetEnvironmentVariableW(L"LLCL_RUNTIME_DIR", env, 4096) > 0)
        glob_java_from(a, env, L"*");
}

static void add_common_javas(wsa* a) {
    add_runtime_javas(a);
    wchar_t env[4096];
    const wchar_t* pf = NULL;
    const wchar_t* pfx = NULL;
    wchar_t buf1[4096], buf2[4096], buf3[4096];
    if (GetEnvironmentVariableW(L"ProgramFiles", buf1, 4096) > 0) pf = buf1;
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", buf2, 4096) > 0) pfx = buf2;
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf3, 4096) > 0) {
        wchar_t* jdks = path_join(buf3, L"Programs");
        wchar_t* jdks2 = path_join(jdks, L"Java");
        glob_java_from(a, jdks2, L"*");
        free(jdks); free(jdks2);
        wchar_t* jdks3 = path_join(buf3, L".jdks");
        glob_java_from(a, jdks3, L"*"); /* IntelliJ downloaded JDKs */
        free(jdks3);
    }
    if (GetEnvironmentVariableW(L"JAVA_HOME", env, 4096) > 0) add_path_java(a, env);
    wsa dirs;
    memset(&dirs, 0, sizeof(dirs));
    if (pf) {
        wsa_add(&dirs, wcat3(pf, L"\\Java", L""));
        wsa_add(&dirs, wcat3(pf, L"\\Eclipse Adoptium", L""));
        wsa_add(&dirs, wcat3(pf, L"\\Microsoft", L""));
        wsa_add(&dirs, wcat3(pf, L"\\Zulu", L""));
        wsa_add(&dirs, wcat3(pf, L"\\Amazon Corretto", L""));
        wsa_add(&dirs, wcat3(pf, L"\\Semeru", L""));
    }
    if (pfx) {
        wsa_add(&dirs, wcat3(pfx, L"\\Java", L""));
        wsa_add(&dirs, wcat3(pfx, L"\\Eclipse Adoptium", L""));
        wsa_add(&dirs, wcat3(pfx, L"\\Microsoft", L""));
    }
    wsa jdirs;
    memset(&jdirs, 0, sizeof(jdirs));
    for (size_t i = 0; i < dirs.n; i++) {
        wchar_t pat[4096];
        _snwprintf_s(pat, 4096, _TRUNCATE, L"%s\\*", dirs.v[i]);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                wchar_t* d = path_join(dirs.v[i], fd.cFileName);
                wsa_add(&jdirs, d);
                free(d);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (size_t i = 0; i < jdirs.n; i++) add_path_java(a, jdirs.v[i]);
    wsa_free(&jdirs);
    wsa_free(&dirs);
    /* PATH */
    wchar_t pathbuf[32768];
    if (GetEnvironmentVariableW(L"PATH", pathbuf, 32768) > 0) {
        wchar_t* ctx = NULL;
        wchar_t* tok = wcstok_s(pathbuf, L";", &ctx);
        while (tok) {
            if (*tok) add_path_java(a, tok);
            tok = wcstok_s(NULL, L";", &ctx);
        }
    }
}

/* ---------------- detect ---------------- */
static int cmp_javas(const void* a, const void* b) {
    const javaitem* x = (const javaitem*)a;
    const javaitem* y = (const javaitem*)b;
    if (x->is64 != y->is64) return y->is64 - x->is64;
    if (x->major != y->major) return y->major - x->major;
    int c = _wcsicmp(y->fullver, x->fullver);
    if (c) return c;
    return _wcsicmp(x->path, y->path);
}

int java_verify(const wchar_t* java_exe, wchar_t* fullver, size_t cap, int* major, int* is64) {
    char out[8192];
    if (!run_capture(java_exe, L"-version", out, sizeof(out), 8000)) return 0;
    wchar_t fv[128] = L"";
    int mj = 0;
    if (!parse_java_version(out, fv, 128, &mj)) return 0;
    if (fullver && cap) wcscpy_s(fullver, cap, fv);
    if (major) *major = mj;
    if (is64) *is64 = pe_is64(java_exe);
    return 1;
}

int detect_javas(javaitem** out, int* n) {
    *out = NULL; *n = 0;
    wsa cands;
    memset(&cands, 0, sizeof(cands));
    add_common_javas(&cands);
    wchar_t root[128];
    _snwprintf_s(root, 128, _TRUNCATE, L"SOFTWARE\\JavaSoft");
    add_registry_javas(&cands, root);
    if (is_os64()) {
        _snwprintf_s(root, 128, _TRUNCATE, L"SOFTWARE\\WOW6432Node\\JavaSoft");
        add_registry_javas(&cands, root);
    }
    /* 去重：同一 java.exe 可能同时出现在 runtime、JAVA_HOME 和 PATH 中，
     * 否则会对同一文件重复执行 java -version（每次 ~0.2-1 秒）。 */
    wsa uniq;
    memset(&uniq, 0, sizeof(uniq));
    for (size_t i = 0; i < cands.n; i++) {
        if (!wsa_has(&uniq, cands.v[i])) wsa_add(&uniq, cands.v[i]);
    }
    wsa_free(&cands);
    cands = uniq;

    javaitem* list = NULL;
    int cnt = 0, cap = 0;
    for (size_t i = 0; i < cands.n && cnt < 48; i++) {
        wchar_t fv[128];
        int mj = 0, i64 = 1;
        if (!java_verify(cands.v[i], fv, 128, &mj, &i64)) continue;
        if (cnt == cap) { cap = cap ? cap * 2 : 16; list = (javaitem*)xrealloc(list, cap * sizeof(javaitem)); }
        list[cnt].path = wdup(cands.v[i]);
        list[cnt].fullver = wdup(fv);
        list[cnt].major = mj;
        list[cnt].is64 = i64;
        cnt++;
    }
    wsa_free(&cands);
    if (cnt > 1) qsort(list, (size_t)cnt, sizeof(javaitem), cmp_javas);
    *out = list;
    *n = cnt;
    return 1;
}

void javas_free(javaitem* a, int n) {
    for (int i = 0; i < n; i++) { free(a[i].path); free(a[i].fullver); }
    free(a);
}

const wchar_t* java_major_req_name(int major) {
    switch (major) {
    case 8: return L"Java 8";
    case 11: return L"Java 11";
    case 16: return L"Java 16";
    case 17: return L"Java 17";
    case 21: return L"Java 21";
    case 25: return L"Java 25";
    default: return L"Java ?";
    }
}
