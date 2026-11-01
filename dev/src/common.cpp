/* LLcl - common.c : string / path / timing helpers */
#include "llcl.h"

wchar_t* u2w(const char* s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* w = (wchar_t*)xalloc((size_t)n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
char* w2u(const wchar_t* s) {
    if (!s) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char* c = (char*)xalloc((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, s, -1, c, n, NULL, NULL);
    return c;
}
wchar_t* wstrdup_utf8(const char* s) { return u2w(s); }
wchar_t* wdup(const wchar_t* s) {
    if (!s) return NULL;
    size_t n = wcslen(s) + 1;
    wchar_t* w = (wchar_t*)xalloc(n * sizeof(wchar_t));
    wcscpy_s(w, n, s);
    return w;
}
char* udup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* c = (char*)xalloc(n);
    memcpy(c, s, n);
    return c;
}
void* xalloc(size_t n) {
    void* p = calloc(1, n ? n : 1);
    if (!p) { MessageBoxW(NULL, L"内存不足", L"LLcl", MB_OK | MB_ICONERROR); ExitProcess(1); }
    return p;
}
void* xrealloc(void* p, size_t n) {
    void* q = realloc(p, n ? n : 1);
    if (!q) { MessageBoxW(NULL, L"内存不足", L"LLcl", MB_OK | MB_ICONERROR); ExitProcess(1); }
    return q;
}
wchar_t* wcat3(const wchar_t* a, const wchar_t* b, const wchar_t* c) {
    size_t la = a ? wcslen(a) : 0, lb = b ? wcslen(b) : 0, lc = c ? wcslen(c) : 0;
    wchar_t* r = (wchar_t*)xalloc((la + lb + lc + 1) * sizeof(wchar_t));
    if (la) wcscpy_s(r, la + lb + lc + 1, a);
    if (lb) wcscat_s(r, la + lb + lc + 1, b);
    if (lc) wcscat_s(r, la + lb + lc + 1, c);
    return r;
}
int weq(const wchar_t* a, const wchar_t* b) { return a && b && wcscmp(a, b) == 0; }
int weqi(const wchar_t* a, const wchar_t* b) { return a && b && _wcsicmp(a, b) == 0; }
int wstartswith(const wchar_t* s, const wchar_t* pre) {
    if (!s || !pre) return 0;
    return _wcsnicmp(s, pre, wcslen(pre)) == 0;
}

/* ---- wide string builder ---- */
void wsb_init(wsb* b) { memset(b, 0, sizeof(*b)); }
void wsb_free(wsb* b) { free(b->b); b->b = NULL; b->n = b->cap = 0; }
static void wsb_reserve(wsb* b, size_t need) {
    if (b->cap >= need) return;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) nc *= 2;
    b->b = (wchar_t*)xrealloc(b->b, nc * sizeof(wchar_t));
    b->cap = nc;
}
void wsb_add(wsb* b, const wchar_t* s) {
    if (!s) return;
    size_t l = wcslen(s);
    wsb_reserve(b, b->n + l + 1);
    memcpy(b->b + b->n, s, (l + 1) * sizeof(wchar_t));
    b->n += l;
}
void wsb_addc(wsb* b, wchar_t c) {
    wsb_reserve(b, b->n + 2);
    b->b[b->n++] = c;
    b->b[b->n] = 0;
}
void wsb_addf(wsb* b, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = _vscwprintf(fmt, ap);
    va_end(ap);
    if (need < 0) return;
    wsb_reserve(b, b->n + (size_t)need + 1);
    va_start(ap, fmt);
    _vsnwprintf_s(b->b + b->n, b->cap - b->n, (size_t)need, fmt, ap);
    va_end(ap);
    b->n += (size_t)need;
    b->b[b->n] = 0;
}
wchar_t* wsb_take(wsb* b) {
    wchar_t* r = b->b ? b->b : (wchar_t*)xalloc(sizeof(wchar_t));
    memset(b, 0, sizeof(*b));
    return r;
}

/* ---- wide string array ---- */
void wsa_addown(wsa* a, wchar_t* s) {
    if (!s) return;
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->v = (wchar_t**)xrealloc(a->v, a->cap * sizeof(wchar_t*));
    }
    a->v[a->n++] = s;
}
void wsa_add(wsa* a, const wchar_t* s) { if (s) wsa_addown(a, wdup(s)); }
int wsa_has(wsa* a, const wchar_t* s) {
    for (size_t i = 0; i < a->n; i++)
        if (weqi(a->v[i], s)) return 1;
    return 0;
}
void wsa_free(wsa* a) {
    for (size_t i = 0; i < a->n; i++) free(a->v[i]);
    free(a->v);
    a->v = NULL; a->n = a->cap = 0;
}

/* ---- timing ---- */
uint64_t now_us(void) {
    static LARGE_INTEGER freq;
    static int once = 0;
    if (!once) { QueryPerformanceFrequency(&freq); once = 1; }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000ULL / freq.QuadPart);
}
void get_stamp(wchar_t* out, size_t n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf_s(out, n, _TRUNCATE, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
}

/* ---- paths / fs ---- */
int file_exists(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
int dir_exists(const wchar_t* p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
wchar_t* path_join(const wchar_t* a, const wchar_t* b) {
    size_t la = wcslen(a), lb = wcslen(b);
    int need_slash = la && (a[la - 1] != L'\\' && a[la - 1] != L'/');
    wchar_t* r = (wchar_t*)xalloc((la + lb + 2) * sizeof(wchar_t));
    wcscpy_s(r, la + lb + 2, a);
    if (need_slash) wcscat_s(r, la + lb + 2, L"\\");
    wcscat_s(r, la + lb + 2, b);
    return r;
}
wchar_t* path_join3(const wchar_t* a, const wchar_t* b, const wchar_t* c) {
    wchar_t* ab = path_join(a, b);
    wchar_t* r = path_join(ab, c);
    free(ab);
    return r;
}
const wchar_t* path_base(const wchar_t* p) {
    const wchar_t* s = p;
    const wchar_t* last = p;
    while (*s) { if (*s == L'\\' || *s == L'/') last = s + 1; s++; }
    return last;
}
void strip_slash(wchar_t* p) {
    size_t l = wcslen(p);
    /* Keep drive roots ("C:\") and UNC share roots intact. */
    while (l > 1 && (p[l - 1] == L'\\' || p[l - 1] == L'/')) {
        if (l == 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) break;
        p[--l] = 0;
    }
}
int ensure_dir(const wchar_t* p) {
    if (dir_exists(p)) return 1;
    if (CreateDirectoryW(p, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}
int ensure_dir_rec(const wchar_t* p) {
    wchar_t tmp[2048];
    if (!p || !*p) return 0;
    wcscpy_s(tmp, 2048, p);
    strip_slash(tmp);
    size_t n = wcslen(tmp);
    for (size_t i = 0; i < n; i++) {
        if (tmp[i] == L'\\' || tmp[i] == L'/') {
            wchar_t save = tmp[i];
            tmp[i] = 0;
            if (*tmp) { if (!dir_exists(tmp)) CreateDirectoryW(tmp, NULL); }
            tmp[i] = save;
        }
    }
    return ensure_dir(tmp);
}
wchar_t* get_exe_dir(void) {
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(NULL, buf, 4096);
    if (n == 0 || n >= 4096) return wdup(L".");
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (slash) *slash = 0;
    return wdup(buf);
}
wchar_t* abs_path(const wchar_t* p) {
    wchar_t buf[4096];
    DWORD n = GetFullPathNameW(p, 4096, buf, NULL);
    if (n == 0 || n >= 4096) return wdup(p);
    return wdup(buf);
}

/* ---- system info ---- */
int is_os64(void) {
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    return si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
           si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ||
           si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64;
}
void os_ver_numbers(int* major, int* minor, int* build) {
    static int cached_major = 0, cached_minor = 0, cached_build = 0;
    typedef LONG(WINAPI* fnRtlGetVersion)(PRTL_OSVERSIONINFOW);
    if (!cached_major) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        fnRtlGetVersion f = ntdll ? (fnRtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion") : NULL;
        if (f) {
            RTL_OSVERSIONINFOW vi;
            memset(&vi, 0, sizeof(vi));
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (f(&vi) == 0) {
                cached_major = (int)vi.dwMajorVersion;
                cached_minor = (int)vi.dwMinorVersion;
                cached_build = (int)vi.dwBuildNumber;
            }
        }
        if (!cached_major) { cached_major = 6; cached_minor = 1; cached_build = 7601; }
    }
    if (major) *major = cached_major;
    if (minor) *minor = cached_minor;
    if (build) *build = cached_build;
}
int os_ver_major(void) {
    int major = 0;
    os_ver_numbers(&major, NULL, NULL);
    return major;
}
