/* LLcl - javadl.c : 自动下载 Java（Adoptium Temurin，清华镜像优先，Adoptium API 回退）。
 * 大 zip 用系统 tar.exe（Win10 1803+，bsdtar 支持 zip）流式解压，内存占用极小；
 * 老系统（无 tar）回退 zip_extract（会把 zip 读入内存，能用但峰值内存高）。
 */
#include "llcl.h"
#define JSMN_HEADER
#include "jsmn.h"

#define TUNA_ROOT L"https://mirrors.tuna.tsinghua.edu.cn/Adoptium"
#define ADPT_ROOT L"https://api.adoptium.net/v3/binary/latest"

static wchar_t* runtime_root(void) {
    wchar_t* ed = get_exe_dir();
    wchar_t* rt = path_join(ed, L"runtime");
    free(ed);
    ensure_dir_rec(rt);
    return rt;
}

/* 镜像目录页里取最后一个 .zip 文件名（清华镜像每个目录通常只保留最新版） */
static wchar_t* find_zip_name(HINTERNET inet, int major, int is64) {
    wsb url; wsb_init(&url);
    wsb_add(&url, TUNA_ROOT);
    wsb_addf(&url, L"/%d/jre/%s/windows/", major, is64 ? L"x64" : L"x32");
    unsigned char* data = NULL; size_t dlen = 0; int st = 0;
    int got = net_get(inet, url.b, &data, &dlen, &st);
    wsb_free(&url);
    if (!got || st != 200 || !data) { free(data); return NULL; }
    char* best = NULL;
    char* p = (char*)data;
    while ((p = strstr(p, "href=\"")) != NULL) {
        char* s = p + 6;
        char* e = strchr(s, '"');
        if (e && e - s >= 4 && e - s < 200 && memcmp(e - 4, ".zip", 4) == 0) {
            free(best);
            best = (char*)xalloc(e - s + 1);
            memcpy(best, s, e - s);
            best[e - s] = 0;
        }
        p = s;
    }
    free(data);
    wchar_t* w = best ? u2w(best) : NULL;
    free(best);
    return w;
}

/* 用 tar.exe 解压（流式，内存小）。返回 1 表示 tar 被调用；真正成败由调用方验证。 */
static int extract_with_tar(const wchar_t* zip, const wchar_t* dest) {
    const wchar_t* tar = L"C:\\Windows\\System32\\tar.exe";
    if (!file_exists(tar)) return 0;
    wsb args; wsb_init(&args);
    wsb_addf(&args, L"-xf \"%s\" -C \"%s\"", zip, dest);
    char out[256];
    int ran = run_capture(tar, args.b, out, sizeof(out), 180000);
    wsb_free(&args);
    return ran;
}

/* 在指定 runtime 目录枚举顶层子目录，找 bin\java.exe 并验证 major 符合。
   优先精确匹配；否则选 ">= major" 中 major 最小的（Java 向后兼容）。
   返回完整 java.exe 路径（malloc），找不到返回 NULL。 */
static wchar_t* find_in_rt(const wchar_t* rt, int major, int is64) {
    wchar_t* found = NULL;
    wchar_t* best = NULL;
    int bestMajor = -1;
    wchar_t pat[4096];
    wchar_t* sub = wcat3(rt, L"\\*", L"");
    wcscpy_s(pat, 4096, sub);
    free(sub);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t* d = path_join(rt, fd.cFileName);
            wchar_t* j = path_join(d, L"bin\\java.exe");
            free(d);
            if (file_exists(j)) {
                wchar_t fv[128]; int mj = 0, i64 = 1;
                if (java_verify(j, fv, 128, &mj, &i64) && i64 == is64) {
                    if (mj == major) { found = j; break; }
                    if (mj > major && (best == NULL || mj < bestMajor)) {
                        free(best);
                        best = j;
                        bestMajor = mj;
                        continue;
                    }
                }
            }
            free(j);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (found) { free(best); return found; }
    return best;
}

/* 查找已安装的匹配 Java：
   1) <exe目录>\runtime（便携/自动下载位置）
   2) LLCL_RUNTIME_DIR 环境变量（开发时复用已下载 runtime，避免重复下载）
   返回完整 java.exe 路径（malloc），找不到返回 NULL。 */
wchar_t* javadl_find(int major, int is64) {
    wchar_t* found = NULL;
    wchar_t* ed = get_exe_dir();
    wchar_t* rt = path_join(ed, L"runtime");
    free(ed);
    if (dir_exists(rt)) found = find_in_rt(rt, major, is64);
    free(rt);
    if (!found) {
        wchar_t env[4096];
        DWORD n = GetEnvironmentVariableW(L"LLCL_RUNTIME_DIR", env, 4096);
        if (n > 0 && n < 4096 && dir_exists(env))
            found = find_in_rt(env, major, is64);
    }
    return found;
}

/* 下载并安装指定 major 的 Java */
int javadl_install(int major, int is64, dl_progress* prog, wchar_t* err, size_t errlen) {
    wchar_t* existing = javadl_find(major, is64);
    if (existing) { free(existing); return 1; }

    HINTERNET inet;
    if (!net_init(&inet)) { _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败"); return 0; }
    wchar_t* rt = runtime_root();

    wchar_t* zipname = find_zip_name(inet, major, is64);
    wchar_t* zippath = path_join(rt, zipname ? zipname : L"java_jre.zip");
    int ok = 0;

    if (zipname) {
        wsb u; wsb_init(&u);
        wsb_add(&u, TUNA_ROOT);
        wsb_addf(&u, L"/%d/jre/%s/windows/%s", major, is64 ? L"x64" : L"x32", zipname);
        wchar_t* url = wsb_take(&u);
        wsb_free(&u);
        if (prog) { wcsncpy_s(prog->cur, 256, zipname, _TRUNCATE); InterlockedIncrement(&prog->active); }
        ok = net_download_file(inet, url, zippath, NULL, NULL);
        if (prog) InterlockedDecrement(&prog->active);
        free(url);
    }
    /* 镜像失败/无文件名：回退 Adoptium API（跟随重定向到 GitHub，版本号由 API 决定）。
     * 优先 JRE；新版 Java（如 25）Adoptium 不发布独立 JRE，回退 JDK（同样可用）。 */
    if (!ok) {
        static const wchar_t* img[2] = { L"jre", L"jdk" };
        for (int k = 0; k < 2 && !ok; k++) {
            wsb u; wsb_init(&u);
            wsb_add(&u, ADPT_ROOT);
            wsb_addf(&u, L"/%d/ga/windows/%s/%s/hotspot/normal/eclipse",
                     major, is64 ? L"x64" : L"x86", img[k]);
            wchar_t* url = wsb_take(&u);
            wsb_free(&u);
            if (prog) {
                _snwprintf_s(prog->cur, 256, _TRUNCATE, L"从 Adoptium 下载 %s", img[k]);
                InterlockedIncrement(&prog->active);
            }
            ok = net_download_file(inet, url, zippath, NULL, NULL);
            if (prog) InterlockedDecrement(&prog->active);
            free(url);
        }
    }
    InternetCloseHandle(inet);
    if (!ok) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"Java 下载失败（网络不可达），可稍后重试");
        free(zipname); free(zippath); free(rt);
        return 0;
    }

    /* 解压：优先 tar（Win10+，内存小），否则 zip_extract（老系统，峰值内存高） */
    int ran = extract_with_tar(zippath, rt);
    if (!ran) {
        int cnt = 0;
        zip_extract_prefix(zippath, NULL, rt, &cnt);
    }
    DeleteFileW(zippath);

    wchar_t* j = javadl_find(major, is64);
    free(zipname); free(zippath); free(rt);
    if (!j) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"Java 解压后未找到可用的 java.exe");
        return 0;
    }
    free(j);
    return 1;
}
