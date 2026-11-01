/* LLcl - diskclean.c : 磁盘清理 —— 扫描无用文件（日志/崩溃报告/JVM 转储/缓存），
 * 递归统计与删除（只清理游戏目录内的固定类别，路径显式构造） */
#include "llcl.h"

wchar_t* fmt_bytes(uint64_t b) {
    wchar_t* s = (wchar_t*)xalloc(64 * sizeof(wchar_t));
    if (b < 1024) _snwprintf_s(s, 64, _TRUNCATE, L"%llu B", (unsigned long long)b);
    else if (b < 1024ULL * 1024) _snwprintf_s(s, 64, _TRUNCATE, L"%.1f KB", (double)b / 1024.0);
    else if (b < 1024ULL * 1024 * 1024) _snwprintf_s(s, 64, _TRUNCATE, L"%.1f MB", (double)b / (1024.0 * 1024.0));
    else _snwprintf_s(s, 64, _TRUNCATE, L"%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
    return s;
}

void ccat_free(ccat* c, int n) {
    for (int i = 0; i < n; i++) { free(c[i].label); free(c[i].root); }
    free(c);
}

typedef struct {
    uint64_t bytes;
    int files, dirs;
    clean_progress cb;
    void* user;
    int last_report;
} scanst;

static void scan_prog(scanst* st) {
    st->files++;
    if (st->cb && (st->files - st->last_report) >= 64) {
        st->last_report = st->files;
        st->cb(st->user, st->files, st->bytes);
    }
}

static void scan_dir(const wchar_t* dir, scanst* st) {
    wchar_t pat[4096];
    _snwprintf_s(pat, 4096, _TRUNCATE, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t* fp = path_join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* 不跟随 junction/symlink，避免清理扫描陷入环或误删链接目标 */
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { free(fp); continue; }
            st->dirs++;
            scan_dir(fp, st);
        } else {
            ULARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            st->bytes += sz.QuadPart;
            scan_prog(st);
        }
        free(fp);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int disk_scan(const wchar_t* gamedir, ccat** out, int* n, clean_progress cb, void* user) {
    *out = NULL; *n = 0;
    int cap = 0, cnt = 0;
    ccat* list = NULL;
    struct { const wchar_t* sub; const wchar_t* label; } dirs[] = {
        { L"logs", L"日志 (logs\\)" },
        { L"crash-reports", L"崩溃报告 (crash-reports\\)" },
        { L"downloads", L"下载缓存 (downloads\\)" },
        { L"tmp", L"启动器临时 (tmp\\)" },
        { L"webcache", L"网页缓存 (webcache\\)" },
    };
    for (int i = 0; i < 5; i++) {
        wchar_t* root = path_join(gamedir, dirs[i].sub);
        if (!dir_exists(root)) { free(root); continue; }
        if (cnt == cap) { cap = cap ? cap * 2 : 8; list = (ccat*)xrealloc(list, cap * sizeof(ccat)); }
        memset(&list[cnt], 0, sizeof(ccat));
        list[cnt].label = wdup(dirs[i].label);
        list[cnt].root = root;
        list[cnt].pattern = 0;
        scanst st;
        memset(&st, 0, sizeof(st));
        st.cb = cb; st.user = user;
        scan_dir(root, &st);
        list[cnt].bytes = st.bytes;
        list[cnt].files = st.files;
        list[cnt].dirs = st.dirs;
        cnt++;
    }
    /* JVM crash remnants: hs_err_pid*.log / *.hprof in game root */
    {
        wchar_t pat[4096];
        const wchar_t* pats[] = { L"hs_err_pid*.log", L"*.hprof" };
        for (int k = 0; k < 2; k++) {
            _snwprintf_s(pat, 4096, _TRUNCATE, L"%s\\%s", gamedir, pats[k]);
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(pat, &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            int got = 0;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                wchar_t* fp = path_join(gamedir, fd.cFileName);
                ULARGE_INTEGER sz;
                sz.LowPart = fd.nFileSizeLow;
                sz.HighPart = fd.nFileSizeHigh;
                if (!got) {
                    if (cnt == cap) { cap = cap ? cap * 2 : 8; list = (ccat*)xrealloc(list, cap * sizeof(ccat)); }
                    memset(&list[cnt], 0, sizeof(ccat));
                    list[cnt].label = wdup(L"JVM 崩溃残留 (hs_err_*.log / *.hprof)");
                    list[cnt].root = wdup(gamedir);
                    list[cnt].pattern = k + 1;   /* 1=hs_err 2=hprof */
                    got = 1;
                }
                list[cnt].bytes += sz.QuadPart;
                list[cnt].files++;
                free(fp);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
            if (got) cnt++;
        }
    }
    *out = list;
    *n = cnt;
    return 1;
}

/* ---------------- clean ---------------- */
static uint64_t del_dir(const wchar_t* dir, clean_progress cb, void* user, int* files, uint64_t* bytes) {
    wchar_t pat[4096];
    _snwprintf_s(pat, 4096, _TRUNCATE, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { RemoveDirectoryW(dir); return 0; }
    uint64_t freed = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        wchar_t* fp = path_join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                RemoveDirectoryW(fp);   /* 删除链接本身，不进入目标 */
                free(fp);
                continue;
            }
            freed += del_dir(fp, cb, user, files, bytes);
            RemoveDirectoryW(fp);
            free(fp);
        } else {
            ULARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            if (DeleteFileW(fp)) {
                freed += sz.QuadPart;
                (*files)++;
                *bytes += sz.QuadPart;
                if (cb && (*files % 128) == 0) cb(user, *files, *bytes);
            }
            free(fp);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir);
    return freed;
}

uint64_t disk_clean_one(ccat* c, clean_progress cb, void* user) {
    if (c->pattern == 0) {
        if (!dir_exists(c->root)) return 0;
        int files = 0;
        uint64_t bytes = 0;
        return del_dir(c->root, cb, user, &files, &bytes);
    }
    /* pattern mode: delete only the category's own pattern (no recursion) */
    uint64_t freed = 0;
    const wchar_t* pats[] = { L"hs_err_pid*.log", L"*.hprof" };
    int k0 = (c->pattern == 2) ? 1 : 0;
    for (int k = k0; k <= k0; k++) {
        wchar_t pat[4096];
        _snwprintf_s(pat, 4096, _TRUNCATE, L"%s\\%s", c->root, pats[k]);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t* fp = path_join(c->root, fd.cFileName);
            ULARGE_INTEGER sz;
            sz.LowPart = fd.nFileSizeLow;
            sz.HighPart = fd.nFileSizeHigh;
            if (DeleteFileW(fp)) freed += sz.QuadPart;
            free(fp);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return freed;
}
