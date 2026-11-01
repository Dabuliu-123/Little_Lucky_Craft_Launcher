/* LLcl - ui.c : Win32 主窗口 + 设置/磁盘清理/下载/登录对话框 + 后台线程 + 日志
 * 布局说明：所有控件坐标按 96DPI 基准书写，由 SX()/SY() 按系统 DPI 等比缩放，
 * 避免高 DPI 屏幕上"字大框小、文字被裁切"。 */
#include "llcl.h"
#define JSMN_HEADER
#include "jsmn.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

HWND g_main = NULL, g_log = NULL, g_status = NULL, g_list = NULL, g_btn_launch = NULL;
static HWND g_acc_label = NULL;
static HINSTANCE g_hinst;
static HFONT g_font;
static cfg g_cfg;
static ventry* g_vers = NULL;
static int g_nvers = 0;
static javaitem* g_javas = NULL;
static int g_njavas = 0;
static int g_java_sel = -1;
static vinfo g_cur;                 /* resolved current version */
static int g_cur_ok = 0;
static HWND g_settings = NULL;
static HWND g_cleanup = NULL;
static HWND g_mpwin = NULL;
static HWND g_mp_list = NULL;
static HWND g_net_label = NULL;
static srv_entry* g_mp_servers = NULL;
static int g_mp_n = 0;
static int g_mp_busy = 0;
static int g_scanning = 0, g_cleaning = 0, g_launching = 0;
static wchar_t* g_cfgpath;
static ccat* g_cats = NULL;
static int g_ncats = 0;

/* DPI 缩放（百分比，96DPI=100） */
static int g_dpix = 100, g_dpiy = 100;
static int SX(int x) { return MulDiv(x, g_dpix, 100); }
static int SY(int y) { return MulDiv(y, g_dpiy, 100); }

/* ---- control ids ---- */
#define IDC_LIST 2001
#define IDC_BTN_LAUNCH 2002
#define IDC_BTN_SETTINGS 2003
#define IDC_BTN_CLEAN 2004
#define IDC_BTN_JAVADETECT 2005
#define IDC_HINT 2006
#define IDC_LOG 2007
#define IDC_BTN_DOWNLOAD 2008
#define IDC_BTN_AUTH 2009
#define IDC_BTN_CONTENT 2010
#define IDC_BTN_MODPACK 2011
#define IDC_MAIN_PLAYER 2012
#define IDC_BTN_OPENDIR 2013
#define IDC_BTN_VERMGR 2014
#define IDC_BTN_MULTI 2015
#define IDC_S_COMBO 2101
#define IDC_S_PATH 2102
#define IDC_S_BROWSE 2103
#define IDC_S_REDETECT 2104
#define IDC_S_MEM 2105
#define IDC_S_MEMHINT 2106
#define IDC_S_PLAYER 2107
#define IDC_S_GAMEDIR 2108
#define IDC_S_BROWSEGD 2109
#define IDC_S_OK 2110
#define IDC_S_CANCEL 2111
#define IDC_S_ISOLATE 2112
#define IDC_S_MEMOPT 2113
#define IDC_S_DLTHREAD 2114
#define IDC_C_LIST 2201
#define IDC_C_SCAN 2202
#define IDC_C_CLEAN 2203
#define IDC_C_STATUS 2204
#define IDC_C_CLOSE 2205
#define IDC_D_VER 2301
#define IDC_D_OK 2302
#define IDC_D_PROG 2303
#define IDC_D_STATUS 2304
#define IDC_D_CLOSE 2305
#define IDC_D_BAR 2306
#define IDC_D_LOG 2307
#define IDC_D_LOADER 2308
#define IDC_D_CUSTOM 2309
#define IDC_D_TYPE 2310   /* 下载类型：原版/加载器 */
#define IDC_D_LOADERKIND 2311  /* 加载器类型：Fabric/Forge/NeoForge/Quilt */
/* loader (模组加载器) dialog */
#define IDC_LD_MCVER 2701
#define IDC_LD_KIND 2702
#define IDC_LD_FETCH 2703
#define IDC_LD_LIST 2704
#define IDC_LD_INSTALL 2705
#define IDC_LD_STATUS 2706
#define IDC_LD_CLOSE 2707
#define IDC_LD_BAR 2708
#define WM_APP_LD_DONE (WM_APP + 12)
#define IDC_A_STATUS 2401
#define IDC_A_DEVICE 2402
#define IDC_A_TOKEN 2403
#define IDC_A_LOGOUT 2404
#define IDC_A_EDIT 2405
#define IDC_A_VERIFY 2406
#define IDT_DL 2501
/* content (下载中心) dialog */
#define IDC_CT_TYPE 2601
#define IDC_CT_LOADER 2602
#define IDC_CT_QUERY 2603
#define IDC_CT_SEARCH 2604
#define IDC_CT_PLIST 2605
#define IDC_CT_VLIST 2606
#define IDC_CT_DOWNLOAD 2607
#define IDC_CT_STATUS 2608
#define IDC_CT_CLOSE 2609
#define IDC_CT_BAR 2610
#define IDC_CT_MCVER 2611
#define IDC_CT_TARGET 2612
#define IDC_CT_LOCALMP 2613
/* version manager (版本管理：模组/光影/数据包) */
#define IDC_VM_TYPE 2801
#define IDC_VM_LIST 2802
#define IDC_VM_ADD 2803
#define IDC_VM_OPEN 2804
#define IDC_VM_TOGGLE 2805
#define IDC_VM_DELETE 2806
#define IDC_VM_REFRESH 2807
#define IDC_VM_STATUS 2808
#define IDC_VM_CLOSE 2809
#define IDC_VM_VER 2810
#define IDC_BTN_MULTI 2015
#define IDC_NET_LABEL 2016
/* ================= 远程联机 / 多人游戏 ================= */
#define IDC_MP_LIST 3001
#define IDC_MP_ADD 3002
#define IDC_MP_EDIT 3003
#define IDC_MP_DELETE 3004
#define IDC_MP_CONNECT 3005
#define IDC_MP_REFRESH 3006
#define IDC_MP_DIRECT 3007
#define IDC_MP_STATUS 3008
#define IDC_MP_CLOSE 3009
#define IDC_MP_LANSCAN 3010
#define IDC_MP_ADDEDIT 3011
/* 远程联机房间控件 */
#define IDC_NET_MODE_D 3101
#define IDC_NET_MODE_R 3102
#define IDC_NET_RELAY 3103
#define IDC_NET_PORT 3104
#define IDC_NET_CREATE 3105
#define IDC_NET_JOIN 3106
#define IDC_NET_STOP 3107
#define IDC_NET_COPY 3108
#define IDC_NET_COPYADDR 3109
#define IDC_NET_STATUS 3110
#define IDC_NET_INFO 3111
#define IDC_NET_INVITE 3112
#define WM_APP_NET_EV (WM_APP + 20)
#define WM_APP_SRV_DONE (WM_APP + 21)
/* modpack custom message */
#define WM_APP_CT_DONE (WM_APP + 9)
#define WM_APP_MP_DONE (WM_APP + 10)
#define WM_APP_JD_DONE (WM_APP + 11)   /* Java 自动下载完成 */

#define WM_APP_JAVA_DONE (WM_APP + 1)
#define WM_APP_SCAN_DONE (WM_APP + 2)
#define WM_APP_SCAN_PROG (WM_APP + 3)
#define WM_APP_CLEAN_DONE (WM_APP + 4)
#define WM_APP_LAUNCH_DONE (WM_APP + 5)
#define WM_APP_DL_READY (WM_APP + 6)
#define WM_APP_DL_DONE (WM_APP + 8)
#define WM_APP_A_CODE (WM_APP + 9)
#define WM_APP_A_DONE (WM_APP + 10)

/* ---------------- log / status ---------------- */
static void log_append(const wchar_t* s) {
    if (!g_log) return;
    wchar_t st[16];
    get_stamp(st, 16);
    wsb b;
    wsb_init(&b);
    int old = GetWindowTextLengthW(g_log);
    if (old > 0) {
        wchar_t* cur = (wchar_t*)xalloc(((size_t)old + 1) * sizeof(wchar_t));
        GetWindowTextW(g_log, cur, old + 1);
        wsb_add(&b, cur);
        free(cur);
    }
    wsb_addf(&b, L"[%s] %s\r\n", st, s);
    /* 日志防膨胀：超过 96KB 时丢弃前半 */
    if (b.n > 96 * 1024) {
        wsb t;
        wsb_init(&t);
        wsb_add(&t, b.b + b.n / 2);
        wsb_free(&b);
        b = t;
    }
    SetWindowTextW(g_log, b.b);
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
    wsb_free(&b);
}

/* 读取重定向日志尾部若干行显示到日志窗格（UTF-8，最多 6KB / 最后 14 行） */
static void show_log_tail(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return; }
    DWORD rd = (DWORD)(sz.QuadPart > 6144 ? 6144 : sz.QuadPart);
    LARGE_INTEGER off;
    off.QuadPart = sz.QuadPart - rd;
    SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    char* buf = (char*)xalloc((size_t)rd + 1);
    DWORD got = 0;
    ReadFile(h, buf, rd, &got, NULL);
    buf[got] = 0;
    CloseHandle(h);
    wchar_t* w = u2w(buf);
    free(buf);
    if (!w) { ui_log(L"（Java 输出无法解码，请查看日志文件）"); return; }
    wchar_t* lines[16];
    int n = 0;
    wchar_t* save = NULL;
    for (wchar_t* tok = wcstok_s(w, L"\r\n", &save); tok && n < 16; tok = wcstok_s(NULL, L"\r\n", &save))
        lines[n++] = tok;
    int start = n > 14 ? n - 14 : 0;
    ui_log(L"--- Java 输出（尾部 %d 行） ---", n - start);
    for (int i = start; i < n; i++) ui_log(L"  %s", lines[i]);
    ui_log(L"--- 结束（完整日志: %s） ---", path);
    free(w);
}

void ui_log(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = _vscwprintf(fmt, ap);
    va_end(ap);
    if (need < 0) return;
    wchar_t* buf = (wchar_t*)xalloc(((size_t)need + 1) * sizeof(wchar_t));
    va_start(ap, fmt);
    _vsnwprintf_s(buf, (size_t)need + 1, (size_t)need, fmt, ap);
    va_end(ap);
    log_append(buf);
    free(buf);
}

void ui_status(const wchar_t* fmt, ...) {
    if (!g_status) return;
    va_list ap;
    va_start(ap, fmt);
    int need = _vscwprintf(fmt, ap);
    va_end(ap);
    if (need < 0) return;
    wchar_t* buf = (wchar_t*)xalloc(((size_t)need + 1) * sizeof(wchar_t));
    va_start(ap, fmt);
    _vsnwprintf_s(buf, (size_t)need + 1, (size_t)need, fmt, ap);
    va_end(ap);
    SetWindowTextW(g_status, buf);
    free(buf);
}

/* ---------------- helpers ---------------- */
static void set_font(HWND h) {
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
}

/* 控件创建（坐标按 96DPI 基准，内部自动按 DPI 缩放） */
static HWND mk(DWORD exstyle, const wchar_t* cls, const wchar_t* txt, DWORD style,
               int x, int y, int w, int h, HWND parent, int id) {
    HWND c = CreateWindowExW(exstyle, cls, txt, WS_CHILD | WS_VISIBLE | style,
                             SX(x), SY(y), SX(w), SY(h), parent,
                             (HMENU)(INT_PTR)(LONG_PTR)id, g_hinst, NULL);
    if (c) set_font(c);
    return c;
}

static void browse_folder(HWND owner, HWND edit) {
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[4096];
        if (SHGetPathFromIDListW(pidl, path)) {
            SetWindowTextW(edit, path);
        }
        CoTaskMemFree(pidl);
    }
}

static void refresh_account_label(void) {
    if (!g_acc_label) return;
    wchar_t t[256];
    if (g_cfg.msa_uuid && g_cfg.msa_uuid[0] &&
        ((g_cfg.msa_access && g_cfg.msa_access[0]) || (g_cfg.msa_refresh && g_cfg.msa_refresh[0])))
        _snwprintf_s(t, 256, _TRUNCATE, L"账号：正版 %s（UUID %.8s…）",
                     g_cfg.msa_name ? g_cfg.msa_name : L"?", g_cfg.msa_uuid);
    else
        _snwprintf_s(t, 256, _TRUNCATE, L"账号：离线 %s（可在“账号与登录”或“设置”中改名）",
                     g_cfg.player ? g_cfg.player : L"Steve");
    SetWindowTextW(g_acc_label, t);
}

static void java_hint_text(void) {
    wsb b;
    wsb_init(&b);
    int autoheap = auto_heap_mb();
    int heap = g_cfg.mem_mb > 0 ? g_cfg.mem_mb : autoheap;
    if (g_java_sel >= 0 && g_java_sel < g_njavas) {
        javaitem* j = &g_javas[g_java_sel];
        wsb_addf(&b, L"Java %s（%d 位）", j->fullver, j->is64 ? 64 : 32);
    } else if (g_njavas > 0) {
        wsb_addf(&b, L"Java %s（%d 位）[自动]", g_javas[0].fullver, g_javas[0].is64 ? 64 : 32);
    } else {
        wsb_add(&b, L"未检测到 Java，请在“设置”中指定");
    }
    if (g_cur_ok && g_cur.java_major > 0) {
        int okj = 0;
        for (int i = 0; i < g_njavas; i++)
            if (g_javas[i].major >= g_cur.java_major && g_javas[i].is64) { okj = 1; break; }
        wsb_addf(&b, L"\r\n此版本需 Java %s%s", java_major_req_name(g_cur.java_major), okj ? L"（已就绪）" : L"（未找到!）");
    }
    wsb_addf(&b, L"\r\n内存: %s %dMB", g_cfg.mem_mb > 0 ? L"手动" : L"自动", heap);
    ui_status(b.b);
    wsb_free(&b);
}

static void pick_java_for_version(void) {
    if (!g_cur_ok || g_cur.java_major <= 0) return;
    /* 优先精确匹配；否则选">=需求"中 major 最小的，避免跨度过大。 */
    int best = -1, bestMajor = -1;
    for (int i = 0; i < g_njavas; i++) {
        if (!g_javas[i].is64) continue;
        if (g_javas[i].major < g_cur.java_major) continue;
        if (g_javas[i].major == g_cur.java_major) { g_java_sel = i; return; }
        if (best < 0 || g_javas[i].major < bestMajor) { best = i; bestMajor = g_javas[i].major; }
    }
    if (best >= 0) { g_java_sel = best; return; }
    /* 无兼容版本：回退到 0（最佳） */
    if (g_njavas > 0) g_java_sel = 0;
}

/* ---------------- version list ---------------- */
static void populate_versions(void) {
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_nvers; i++)
        SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)g_vers[i].id);
    if (g_cfg.last_version) {
        int idx = (int)SendMessageW(g_list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)g_cfg.last_version);
        if (idx != LB_ERR) SendMessageW(g_list, LB_SETCURSEL, idx, 0);
    }
    if (SendMessageW(g_list, LB_GETCURSEL, 0, 0) == LB_ERR && g_nvers > 0)
        SendMessageW(g_list, LB_SETCURSEL, 0, 0);
}

static void select_version(int idx) {
    vinfo_free(&g_cur);
    g_cur_ok = 0;
    if (idx < 0 || idx >= g_nvers) return;
    if (!resolve_version(g_cfg.game_dir, g_vers[idx].id, &g_cur)) {
        ui_log(L"版本 %s 解析失败", g_vers[idx].id);
        return;
    }
    g_cur_ok = 1;
    int nc = 0;
    for (size_t k = 0; k < g_cur.nlibs; k++) if (g_cur.libs[k].is_native) nc++;
    ui_log(L"版本 %s: mainClass=%s, 库 %u 个（原生 %u），Java要求=%s, 参数=%s",
           g_cur.id, g_cur.main_class ? g_cur.main_class : L"-",
           (unsigned)g_cur.nlibs, (unsigned)nc,
           g_cur.java_major ? java_major_req_name(g_cur.java_major) : L"未知",
           g_cur.has_modern ? L"arguments" : (g_cur.has_legacy ? L"minecraftArguments" : L"无"));
    pick_java_for_version();
    java_hint_text();
}

/* ---------------- worker threads ---------------- */
typedef struct { javaitem* list; int n; } jres;
static DWORD WINAPI thread_detect(LPVOID p) {
    HWND hwnd = (HWND)p;
    javaitem* list = NULL;
    int n = 0;
    detect_javas(&list, &n);
    jres* res = (jres*)xalloc(sizeof(jres));
    res->list = list;
    res->n = n;
    PostMessageW(hwnd, WM_APP_JAVA_DONE, (WPARAM)res, 0);
    return 0;
}

typedef struct { lresult r; } lres;
typedef struct {
    const vinfo* v;
    wchar_t* gamedir;
    wchar_t* java;
    int mem;
    wchar_t* player;
    wchar_t* uuid;
    wchar_t* access;
    wchar_t* xuid;
    wchar_t* refresh;
    int isolate;
    int mem_opt;
} largs;
static DWORD WINAPI thread_launch(LPVOID p) {
    largs* a = (largs*)p;
    wchar_t* access = a->access ? wdup(a->access) : NULL;
    wchar_t* uuid = a->uuid ? wdup(a->uuid) : NULL;
    wchar_t* xuid = a->xuid ? wdup(a->xuid) : NULL;
    wchar_t* player = wdup(a->player ? a->player : L"Steve");
    /* 有 refresh_token 时在后台先续期（不阻塞 UI），拿新的 access/uuid/xuid；
       失败则退回已保存的 access（可能仍有效），再失败才按离线启动。 */
    if (a->refresh && a->refresh[0]) {
        wchar_t nu[128] = L"", nn[128] = L"", nx[64] = L"", nr[8192] = L"", ne[640] = L"";
        wchar_t na[8192] = L"";
        if (msa_restore(g_cfg.msa_client, a->refresh, nu, 128, nn, 128, nx, 64,
                        na, 8192, nr, 8192, ne, 640)) {
            free(access); free(uuid); free(xuid); free(player);
            access = na[0] ? wdup(na) : NULL;
            free(g_cfg.msa_access); g_cfg.msa_access = na[0] ? wdup(na) : NULL;
            uuid = nu[0] ? wdup(nu) : NULL;
            xuid = nx[0] ? wdup(nx) : NULL;
            player = nn[0] ? wdup(nn) : wdup(L"Steve");
            free(g_cfg.msa_refresh); g_cfg.msa_refresh = nr[0] ? wdup(nr) : NULL;
            free(g_cfg.msa_uuid); g_cfg.msa_uuid = uuid ? wdup(uuid) : NULL;
            free(g_cfg.msa_name); g_cfg.msa_name = nn[0] ? wdup(nn) : NULL;
            free(g_cfg.msa_xuid); g_cfg.msa_xuid = xuid ? wdup(xuid) : NULL;
            free(g_cfg.player); g_cfg.player = wdup(player);
            cfg_save(g_cfgpath, &g_cfg);
            ui_log(L"微软登录已自动续期: %s", player);
        } else {
            ui_log(L"微软登录续期失败（将沿用上次凭据）: %s", ne);
        }
    }
    lres* res = (lres*)xalloc(sizeof(lres));
    launch_game(a->v, a->gamedir, a->java, a->mem, player, uuid,
                access, xuid, a->isolate, a->mem_opt, &res->r);
    vinfo_free((vinfo*)a->v);
    free((vinfo*)a->v);
    free(a->gamedir);
    free(a->java);
    free(a->player);
    free(a->uuid);
    free(a->access);
    free(a->xuid);
    free(a->refresh);
    free(player);
    free(access);
    free(uuid);
    free(xuid);
    free(a);
    PostMessageW(g_main, WM_APP_LAUNCH_DONE, (WPARAM)res, 0);
    return 0;
}

typedef struct { ccat* cats; int n; } sres;
static int scan_prog_cb(void* user, int files, uint64_t bytes) {
    PostMessageW(g_main, WM_APP_SCAN_PROG, (WPARAM)files, (LPARAM)bytes);
    return 0;
}
static DWORD WINAPI thread_scan(LPVOID p) {
    (void)p;
    ccat* cats = NULL;
    int n = 0;
    disk_scan(g_cfg.game_dir, &cats, &n, scan_prog_cb, NULL);
    sres* res = (sres*)xalloc(sizeof(sres));
    res->cats = cats;
    res->n = n;
    PostMessageW(g_main, WM_APP_SCAN_DONE, (WPARAM)res, 0);
    return 0;
}

typedef struct { ccat* cats; int* sel; int nsel; int n; uint64_t freed; } cres;
static int clean_prog_cb(void* user, int files, uint64_t bytes) {
    (void)user; (void)files; (void)bytes;
    return 0;
}
static DWORD WINAPI thread_clean(LPVOID p) {
    cres* arg = (cres*)p;
    cres* res = (cres*)xalloc(sizeof(cres));
    res->freed = 0;
    for (int i = 0; i < arg->nsel; i++) {
        if (arg->sel[i] < 0 || arg->sel[i] >= arg->n) continue;
        res->freed += disk_clean_one(&arg->cats[arg->sel[i]], clean_prog_cb, NULL);
    }
    free(arg->sel);
    free(arg);
    PostMessageW(g_main, WM_APP_CLEAN_DONE, (WPARAM)res, 0);
    return 0;
}

/* ---------------- dialogs ---------------- */
/* 模态窗口栈：支持从一个模态对话框再嵌套打开另一个（如 下载游戏 → 模组加载器）。
 * 每层一个独立的关闭标志，内层关闭不影响外层。 */
#define MODAL_MAX 6
static HWND g_modal_stk[MODAL_MAX];
static int  g_modal_close[MODAL_MAX];
static int  g_modal_top = 0;
static void modal_close(void) { if (g_modal_top) g_modal_close[g_modal_top - 1] = 1; }

static HWND modal_create(const wchar_t* cls, const wchar_t* title, int w, int h) {
    RECT pr;
    HWND rel = g_modal_top ? g_modal_stk[g_modal_top - 1] : g_main;
    GetWindowRect(rel, &pr);
    /* 传入的 w,h 是【客户区】逻辑尺寸：先 DPI 缩放，再用 AdjustWindowRectEx
       补上标题栏与边框，保证控件按客户区布局时不被窗口边界截断 */
    RECT rc = { 0, 0, SX(w), SY(h) };
    AdjustWindowRectEx(&rc, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int ww = rc.right - rc.left, hh = rc.bottom - rc.top;
    int x = pr.left + (pr.right - pr.left - ww) / 2;
    int y = pr.top + (pr.bottom - pr.top - hh) / 2;
    HWND parent = g_modal_top ? g_modal_stk[g_modal_top - 1] : g_main;
    return CreateWindowExW(WS_EX_DLGMODALFRAME, cls, title,
                           WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                           x, y, ww, hh, parent, NULL, g_hinst, NULL);
}

static void modal_run(HWND d) {
    HWND parent = g_modal_top ? g_modal_stk[g_modal_top - 1] : g_main;
    EnableWindow(parent, FALSE);
    int lvl = g_modal_top++;
    g_modal_stk[lvl] = d; g_modal_close[lvl] = 0;
    int running = 1;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); running = 0; break; }
            if (!IsDialogMessageW(d, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (!running) break;
        if (g_modal_close[lvl]) { running = 0; break; }
        WaitMessage();
    }
    g_modal_top--;
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    DestroyWindow(d);
}

/* ---- settings dialog ---- */
static const int mem_vals[] = { 0, 512, 1024, 1536, 2048, 3072, 4096, 6144, 8192 };
#define NMEM (sizeof(mem_vals) / sizeof(mem_vals[0]))

/* 重新扫描版本列表并刷新主窗口列表（游戏目录变化后调用） */
static void rescan_versions(void) {
    for (int i = 0; i < g_nvers; i++) { free(g_vers[i].id); free(g_vers[i].json_path); }
    free(g_vers);
    g_vers = NULL;
    g_nvers = 0;
    vinfo_free(&g_cur);
    g_cur_ok = 0;
    if (g_cfg.game_dir && dir_exists(g_cfg.game_dir))
        scan_versions(g_cfg.game_dir, &g_vers, &g_nvers);
    populate_versions();
    if (g_nvers > 0) select_version((int)SendMessageW(g_list, LB_GETCURSEL, 0, 0));
}

/* 读取设置对话框控件 → 写入 g_cfg → 保存 ini。返回 1 成功 */
static int settings_save(HWND d) {
    /* java combo -> path */
    HWND cb = GetDlgItem(d, IDC_S_COMBO);
    int ci = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (ci != CB_ERR) {
        int ji = (int)SendMessageW(cb, CB_GETITEMDATA, ci, 0);
        if (ji >= 0 && ji < g_njavas) g_java_sel = ji;
    }
    wchar_t path[4096];
    GetWindowTextW(GetDlgItem(d, IDC_S_PATH), path, 4096);
    free(g_cfg.java_path);
    g_cfg.java_path = (path[0] ? wdup(path) : NULL);
    if (g_cfg.java_path && file_exists(g_cfg.java_path)) {
        wchar_t fv[128];
        int mj = 0, i64 = 1;
        if (java_verify(g_cfg.java_path, fv, 128, &mj, &i64)) {
            int matched = -1;
            for (int i = 0; i < g_njavas; i++)
                if (weqi(g_javas[i].path, g_cfg.java_path)) { matched = i; break; }
            if (matched >= 0) g_java_sel = matched;
            else {
                javaitem* nj = (javaitem*)xrealloc(g_javas, ((size_t)g_njavas + 1) * sizeof(javaitem));
                g_javas = nj;
                g_javas[g_njavas].path = wdup(g_cfg.java_path);
                g_javas[g_njavas].fullver = wdup(fv);
                g_javas[g_njavas].major = mj;
                g_javas[g_njavas].is64 = i64;
                g_java_sel = g_njavas;
                g_njavas++;
            }
        }
    }
    HWND mem = GetDlgItem(d, IDC_S_MEM);
    int mi = (int)SendMessageW(mem, CB_GETCURSEL, 0, 0);
    if (mi != CB_ERR) g_cfg.mem_mb = (int)SendMessageW(mem, CB_GETITEMDATA, mi, 0);
    wchar_t pl[256];
    GetWindowTextW(GetDlgItem(d, IDC_S_PLAYER), pl, 256);
    free(g_cfg.player);
    g_cfg.player = wdup(pl[0] ? pl : L"Steve");
    wchar_t gd[4096];
    GetWindowTextW(GetDlgItem(d, IDC_S_GAMEDIR), gd, 4096);
    wchar_t* old_gd = wdup(g_cfg.game_dir ? g_cfg.game_dir : L"");
    free(g_cfg.game_dir);
    g_cfg.game_dir = wdup(gd);
    strip_slash(g_cfg.game_dir);
    g_cfg.version_isolate = (SendMessageW(GetDlgItem(d, IDC_S_ISOLATE), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    g_cfg.mem_opt = (SendMessageW(GetDlgItem(d, IDC_S_MEMOPT), BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
    g_cfg.dl_threads = ((int)SendMessageW(GetDlgItem(d, IDC_S_DLTHREAD), CB_GETCURSEL, 0, 0) == 1) ? 16 : 8;
    g_dl_threads = g_cfg.dl_threads;   /* 立即同步给下载引擎 */
    int ok = cfg_save(g_cfgpath, &g_cfg);
    if (!ok)
        MessageBoxW(g_main, L"保存配置失败（无法写入 LLcl.ini）。\n请检查文件是否被占用或所在目录是否只读。", L"LLcl", MB_OK | MB_ICONWARNING);
    java_hint_text();
    refresh_account_label();
    if (!weq(old_gd ? old_gd : L"", g_cfg.game_dir ? g_cfg.game_dir : L""))
        rescan_versions();
    free(old_gd);
    return ok;
}

static void browse_java_file(HWND owner, HWND edit) {
    wchar_t cur[4096] = L"";
    GetWindowTextW(edit, cur, 4096);
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"java.exe\0java.exe\0\0";
    ofn.lpstrFile = cur;
    ofn.nMaxFile = 4096;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"选择 java.exe";
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(edit, cur);
}

static void settings_fill_combo(HWND d) {
    HWND cb = GetDlgItem(d, IDC_S_COMBO);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_njavas; i++) {
        wsb b;
        wsb_init(&b);
        wsb_addf(&b, L"Java %d (%s) %d位 - %s", g_javas[i].major, g_javas[i].fullver,
                 g_javas[i].is64 ? 64 : 32, g_javas[i].path);
        int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)b.b);
        SendMessageW(cb, CB_SETITEMDATA, idx, i);
        wsb_free(&b);
    }
    int sel = g_java_sel;
    if (sel >= 0) {
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
    } else if (g_njavas > 0) {
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
    }
}

static void settings_mem_hint(HWND d, int heap) {
    wchar_t t[128];
    uint64_t phys = total_phys_bytes();
    wchar_t* psz = fmt_bytes(phys);
    _snwprintf_s(t, 128, _TRUNCATE, L"物理内存 %s\r\n自动推荐 %dMB\r\nXms=%dMB",
                 psz, auto_heap_mb(), auto_xms_mb(auto_heap_mb()));
    free(psz);
    SetWindowTextW(GetDlgItem(d, IDC_S_MEMHINT), t);
    (void)heap;
}

static LRESULT CALLBACK settings_proc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        /* 基准坐标（430×430 客户区），自动 DPI 缩放；标签高度 18，中文不被裁切 */
        mk(0, L"STATIC", L"Java 版本（探测结果）", SS_LEFT, 14, 10, 380, 18, d, -1);
        mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
           14, 30, 386, 200, d, IDC_S_COMBO);
        mk(0, L"STATIC", L"Java 路径", SS_LEFT, 14, 58, 200, 18, d, -1);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
           14, 78, 284, 24, d, IDC_S_PATH);
        mk(0, L"BUTTON", L"浏览", WS_TABSTOP | BS_PUSHBUTTON,
           306, 78, 94, 24, d, IDC_S_BROWSE);
        mk(0, L"BUTTON", L"重新检测", WS_TABSTOP | BS_PUSHBUTTON,
           14, 110, 110, 26, d, IDC_S_REDETECT);
        mk(0, L"STATIC", L"内存分配", SS_LEFT, 14, 146, 200, 18, d, -1);
        mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
           14, 166, 180, 200, d, IDC_S_MEM);
        mk(0, L"STATIC", L"", SS_LEFT, 204, 166, 200, 52, d, IDC_S_MEMHINT);
        mk(0, L"BUTTON", L"内存优化（GC 调优）",
           WS_TABSTOP | BS_AUTOCHECKBOX, 14, 194, 185, 20, d, IDC_S_MEMOPT);
        mk(0, L"STATIC", L"玩家名", SS_LEFT, 14, 222, 200, 18, d, -1);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
           14, 242, 386, 24, d, IDC_S_PLAYER);
        mk(0, L"STATIC", L"游戏目录（.minecraft 所在目录）", SS_LEFT, 14, 274, 380, 18, d, -1);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
           14, 294, 284, 24, d, IDC_S_GAMEDIR);
        mk(0, L"BUTTON", L"浏览", WS_TABSTOP | BS_PUSHBUTTON,
           306, 294, 94, 24, d, IDC_S_BROWSEGD);
        /* 版本隔离独占一行，不再与游戏目录框重叠；长说明放第二行 */
        mk(0, L"BUTTON", L"版本隔离：每个版本独立存档/模组/配置",
           WS_TABSTOP | BS_AUTOCHECKBOX, 14, 324, 386, 22, d, IDC_S_ISOLATE);
        mk(0, L"STATIC", L"数据存于 versions\\<版本>\\instance；assets 与 libraries 仍全局共享",
           SS_LEFT, 30, 346, 380, 16, d, -1);
        /* 下载线程数：8（省内存，默认）/ 16（更快） */
        mk(0, L"STATIC", L"下载线程数", SS_LEFT, 14, 370, 120, 18, d, -1);
        mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
           140, 368, 260, 200, d, IDC_S_DLTHREAD);
        mk(0, L"BUTTON", L"保存", WS_TABSTOP | BS_DEFPUSHBUTTON,
           190, 404, 90, 30, d, IDC_S_OK);
        mk(0, L"BUTTON", L"取消", WS_TABSTOP | BS_PUSHBUTTON,
           290, 404, 90, 30, d, IDC_S_CANCEL);
        SendMessageW(GetDlgItem(d, IDC_S_ISOLATE), BM_SETCHECK,
                     g_cfg.version_isolate ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(GetDlgItem(d, IDC_S_MEMOPT), BM_SETCHECK,
                     g_cfg.mem_opt ? BST_CHECKED : BST_UNCHECKED, 0);
        /* fill */
        SetWindowTextW(GetDlgItem(d, IDC_S_PATH), g_cfg.java_path ? g_cfg.java_path : L"");
        SetWindowTextW(GetDlgItem(d, IDC_S_PLAYER), g_cfg.player ? g_cfg.player : L"");
        SetWindowTextW(GetDlgItem(d, IDC_S_GAMEDIR), g_cfg.game_dir ? g_cfg.game_dir : L"");
        settings_fill_combo(d);
        HWND mem = GetDlgItem(d, IDC_S_MEM);
        for (int i = 0; i < (int)NMEM; i++) {
            wchar_t t[32];
            if (mem_vals[i] == 0) _snwprintf_s(t, 32, _TRUNCATE, L"自动 (推荐 %dMB)", auto_heap_mb());
            else _snwprintf_s(t, 32, _TRUNCATE, L"%d MB", mem_vals[i]);
            int idx = (int)SendMessageW(mem, CB_ADDSTRING, 0, (LPARAM)t);
            SendMessageW(mem, CB_SETITEMDATA, idx, mem_vals[i]);
            if (g_cfg.mem_mb == mem_vals[i]) SendMessageW(mem, CB_SETCURSEL, idx, 0);
        }
        if (SendMessageW(mem, CB_GETCURSEL, 0, 0) == CB_ERR)
            SendMessageW(mem, CB_SETCURSEL, 0, 0);
        settings_mem_hint(d, g_cfg.mem_mb);
        /* 下载线程下拉：8（省内存，默认）/ 16（更快） */
        HWND dlt = GetDlgItem(d, IDC_S_DLTHREAD);
        SendMessageW(dlt, CB_ADDSTRING, 0, (LPARAM)L"8 线程（省内存，默认）");
        SendMessageW(dlt, CB_ADDSTRING, 0, (LPARAM)L"16 线程（更快，占内存）");
        SendMessageW(dlt, CB_SETCURSEL, (g_cfg.dl_threads == 16) ? 1 : 0, 0);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_S_OK) {
            settings_save(d);
            modal_close();
            return 0;
        }
        if (id == IDC_S_CANCEL) { modal_close(); return 0; }
        if (id == IDC_S_BROWSE) {
            browse_java_file(d, GetDlgItem(d, IDC_S_PATH));
            return 0;
        }
        if (id == IDC_S_BROWSEGD) {
            browse_folder(d, GetDlgItem(d, IDC_S_GAMEDIR));
            return 0;
        }
        if (id == IDC_S_REDETECT) {
            ui_log(L"重新检测 Java...");
            HANDLE t = CreateThread(NULL, 65536, thread_detect, g_main, 0, NULL);
            if (t) CloseHandle(t);
            return 0;
        }
        if (id == IDC_S_COMBO) {
            int ci = (int)SendMessageW(GetDlgItem(d, IDC_S_COMBO), CB_GETCURSEL, 0, 0);
            if (ci != CB_ERR) {
                int ji = (int)SendMessageW(GetDlgItem(d, IDC_S_COMBO), CB_GETITEMDATA, ci, 0);
                if (ji >= 0 && ji < g_njavas)
                    SetWindowTextW(GetDlgItem(d, IDC_S_PATH), g_javas[ji].path);
            }
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CTLCOLOREDIT: return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE: modal_close(); return 0;   /* 红叉等同“取消”，不保存 */
    }
    return DefWindowProcW(d, msg, w, l);
}

/* ---- cleanup dialog ---- */
static LRESULT CALLBACK cleanup_proc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        mk(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_EXTENDEDSEL,
           10, 10, 380, 230, d, IDC_C_LIST);
        mk(0, L"STATIC", L"点击“扫描”查找无用文件", SS_LEFT, 10, 248, 380, 20, d, IDC_C_STATUS);
        mk(0, L"BUTTON", L"扫描", WS_TABSTOP | BS_PUSHBUTTON, 10, 276, 90, 30, d, IDC_C_SCAN);
        mk(0, L"BUTTON", L"清理所选", WS_TABSTOP | BS_PUSHBUTTON, 110, 276, 110, 30, d, IDC_C_CLEAN);
        mk(0, L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON, 300, 276, 90, 30, d, IDC_C_CLOSE);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_C_SCAN && !g_scanning) {
            g_scanning = 1;
            SendMessageW(GetDlgItem(d, IDC_C_LIST), LB_RESETCONTENT, 0, 0);
            SetWindowTextW(GetDlgItem(d, IDC_C_STATUS), L"扫描中…");
            EnableWindow(GetDlgItem(d, IDC_C_SCAN), FALSE);
            EnableWindow(GetDlgItem(d, IDC_C_CLEAN), FALSE);
            HANDLE t = CreateThread(NULL, 65536, thread_scan, NULL, 0, NULL);
            if (t) CloseHandle(t);
            return 0;
        }
        if (id == IDC_C_CLEAN && !g_cleaning && !g_scanning) {
            HWND lb = GetDlgItem(d, IDC_C_LIST);
            int count = (int)SendMessageW(lb, LB_GETSELCOUNT, 0, 0);
            if (count <= 0) { MessageBoxW(d, L"请先选中要清理的项目（可多选）", L"LLcl", MB_OK | MB_ICONINFORMATION); return 0; }
            if (count > 64) count = 64;
            int* sel = (int*)xalloc((size_t)count * sizeof(int));
            int nsel = 0;
            for (int i = 0; i < g_ncats && nsel < count; i++) {
                if (SendMessageW(lb, LB_GETSEL, i, 0)) sel[nsel++] = i;
            }
            g_cleaning = 1;
            EnableWindow(GetDlgItem(d, IDC_C_CLEAN), FALSE);
            EnableWindow(GetDlgItem(d, IDC_C_SCAN), FALSE);
            SetWindowTextW(GetDlgItem(d, IDC_C_STATUS), L"清理中…");
            cres* a = (cres*)xalloc(sizeof(cres));
            a->cats = g_cats;
            a->sel = sel;
            a->nsel = nsel;
            a->n = g_ncats;
            HANDLE t = CreateThread(NULL, 65536, thread_clean, a, 0, NULL);
            if (t) CloseHandle(t);
            return 0;
        }
        if (id == IDC_C_CLOSE) { modal_close(); return 0; }
        return 0;
    }
    case WM_CTLCOLORSTATIC: return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CTLCOLOREDIT: return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        if (g_scanning || g_cleaning) {
            SetWindowTextW(GetDlgItem(d, IDC_C_STATUS), L"扫描/清理进行中，完成前请不要关闭窗口");
            return 0;
        }
        modal_close();
        return 0;
    }
    return DefWindowProcW(d, msg, w, l);
}

/* ---------------- 自动下载 Java 后继续启动 ---------------- */
typedef struct {
    int ok;
    int major;
    int is64;
    int vsel;
    wchar_t err[256];
} jd_result;

static DWORD WINAPI jd_thread(LPVOID p) {
    jd_result* r = (jd_result*)p;
    r->err[0] = 0;
    dl_progress prog;
    memset(&prog, 0, sizeof(prog));
    r->ok = javadl_install(r->major, r->is64, &prog, r->err, 256);
    PostMessageW(g_main, WM_APP_JD_DONE, 0, (LPARAM)r);
    return 0;
}

/* Java 向后兼容：major >= 需求的版本可运行（如 Java 21 可跑需求 17 的游戏）。
 * 优先精确匹配；无精确匹配时选">= 需求"中 major 最小的那个，避免大版本跨度过大。 */
static wchar_t* java_pick_compatible(javaitem* list, int n, int major, int is64) {
    int best = -1, bestMajor = -1;
    for (int i = 0; i < n; i++) {
        if (list[i].is64 != is64) continue;
        if (major <= 0) return wdup(list[i].path);   /* 不限定版本：取第一个（已验证排序最优） */
        if (list[i].major < major) continue;
        if (list[i].major == major) return wdup(list[i].path);
        if (best < 0 || list[i].major < bestMajor) { best = i; bestMajor = list[i].major; }
    }
    return best >= 0 ? wdup(list[best].path) : NULL;
}

/* 查找匹配 major+位数的 java.exe（手动指定/检测到/runtime 已下载），返回 wdup 路径或 NULL */
static wchar_t* find_java_for(int major, int is64) {
    if (g_cfg.java_path && file_exists(g_cfg.java_path)) {
        wchar_t fv[128]; int mj = 0, i64 = 1;
        if (java_verify(g_cfg.java_path, fv, 128, &mj, &i64) &&
            (major <= 0 || mj >= major) && i64 == is64)
            return wdup(g_cfg.java_path);
    }
    if (g_njavas > 0) {
        wchar_t* p = java_pick_compatible(g_javas, g_njavas, major, is64);
        if (p) return p;
    }
    if (major > 0) {
        wchar_t* rt = javadl_find(major, is64);
        if (rt) return rt;
    }
    return NULL;
}

/* ---------------- main window ---------------- */
static void do_launch(void) {
    if (g_launching) return;
    int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel >= g_nvers) { MessageBoxW(g_main, L"请先在列表中选择一个版本", L"LLcl", MB_OK | MB_ICONINFORMATION); return; }
    if (!g_cfg.game_dir || !dir_exists(g_cfg.game_dir)) {
        MessageBoxW(g_main, L"游戏目录无效，请在“设置”中指定正确的游戏目录", L"LLcl", MB_OK | MB_ICONINFORMATION);
        return;
    }
    /* 先解析版本，确定所需 Java 版本 */
    if (!g_cur_ok || !weq(g_cur.id, g_vers[sel].id)) select_version(sel);
    if (!g_cur_ok) { MessageBoxW(g_main, L"版本解析失败", L"LLcl", MB_OK | MB_ICONERROR); return; }
    /* 玩家名由“设置/账号与登录”维护，主窗口不再放重复的输入框。 */
    int needMajor = g_cur.java_major > 0 ? g_cur.java_major : 8;
    int need64 = 1;   /* x64 构建，统一用 64 位 Java */

    wchar_t* java = find_java_for(needMajor, need64);
    if (!java) {
        /* 没有匹配的 Java：询问是否自动下载（Temurin，约 40MB，清华镜像） */
        wchar_t ask[512];
        _snwprintf_s(ask, 512, _TRUNCATE,
            L"此版本需要 Java %s（64 位），但没有检测到。\n\n是否现在自动下载并安装？（约 40MB，下载后即可启动）",
            java_major_req_name(needMajor));
        if (MessageBoxW(g_main, ask, L"LLcl", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        ui_status(L"正在下载 Java，请稍候…");
        jd_result* r = (jd_result*)xalloc(sizeof(jd_result));
        r->major = needMajor;
        r->is64 = need64;
        r->vsel = sel;
        g_launching = 1;
        EnableWindow(g_btn_launch, FALSE);
        HANDLE t = CreateThread(NULL, 65536, jd_thread, r, 0, NULL);
        if (t) CloseHandle(t);
        return;
    }

    g_launching = 1;
    EnableWindow(g_btn_launch, FALSE);
    ui_log(L"启动 %s @ %s (堆 %dMB)", g_cur.id, java, g_cfg.mem_mb > 0 ? g_cfg.mem_mb : auto_heap_mb());
    ui_status(L"启动中…（首次启动需解压原生库，请稍候）");
    largs* a = (largs*)xalloc(sizeof(largs));
    a->v = (vinfo*)xalloc(sizeof(vinfo));
    vinfo_clone((vinfo*)a->v, &g_cur);
    a->gamedir = wdup(g_cfg.game_dir);
    a->java = java;
    a->mem = g_cfg.mem_mb;
    int has_online = (g_cfg.msa_refresh && g_cfg.msa_refresh[0]) ||
                     (g_cfg.msa_access && g_cfg.msa_access[0] && g_cfg.msa_uuid && g_cfg.msa_uuid[0]);
    a->player = wdup((has_online && g_cfg.msa_name && g_cfg.msa_name[0])
                     ? g_cfg.msa_name : (g_cfg.player ? g_cfg.player : L"Steve"));
    a->uuid = (has_online && g_cfg.msa_uuid && g_cfg.msa_uuid[0]) ? wdup(g_cfg.msa_uuid) : NULL;
    a->access = (has_online && g_cfg.msa_access && g_cfg.msa_access[0]) ? wdup(g_cfg.msa_access) : NULL;
    a->xuid = (has_online && g_cfg.msa_xuid && g_cfg.msa_xuid[0]) ? wdup(g_cfg.msa_xuid) : NULL;
    a->refresh = (has_online && g_cfg.msa_refresh && g_cfg.msa_refresh[0]) ? wdup(g_cfg.msa_refresh) : NULL;
    a->isolate = g_cfg.version_isolate;
    a->mem_opt = g_cfg.mem_opt;
    /* 512KB：启动线程内会做 refresh_token 续期（msa_restore 使用大缓冲）。 */
    HANDLE t = CreateThread(NULL, 512 * 1024, thread_launch, a, 0, NULL);
    if (t) CloseHandle(t);
}

static void do_detect(void) {
    ui_log(L"检测 Java...");
    ui_status(L"正在检测 Java…");
    HANDLE t = CreateThread(NULL, 65536, thread_detect, g_main, 0, NULL);
    if (t) CloseHandle(t);
}

static void open_settings(void) {
    if (g_settings) { SetForegroundWindow(g_settings); return; }
    g_settings = modal_create(L"LLclSettings", L"设置", 430, 480);
    modal_run(g_settings);
    g_settings = NULL;
    refresh_account_label();
}

static void open_cleanup(void) {
    if (g_cleanup) { SetForegroundWindow(g_cleanup); return; }
    g_cleanup = modal_create(L"LLclCleanup", L"磁盘清理", 420, 348);
    modal_run(g_cleanup);
    g_cleanup = NULL;
}

/* ================= 下载对话框 ================= */
static int ld_version_installed(const wchar_t* gamedir, const wchar_t* id);  /* 定义在本文件下方 */
static HWND g_dlwin = NULL;
static dl_list* g_dl_manifest = NULL;
static int g_dl_installing = 0;

typedef struct {
    dl_list* list;
    wchar_t* gamedir;
    wchar_t* ver;
    wchar_t* custom;
    int kind;          /* 下载类型：-1=原版；否则为 ld_kind（加载器） */
    dl_progress* prog;
    wchar_t err[640];
} dl_ctx;

static void dlog_append(const wchar_t* s) {
    if (!g_dlwin) return;
    HWND e = GetDlgItem(g_dlwin, IDC_D_LOG);
    if (!e) return;
    wchar_t st[16];
    get_stamp(st, 16);
    int old = GetWindowTextLengthW(e);
    if (old > 20000) {
        SendMessageW(e, EM_SETSEL, 0, 6000);
        SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"");
        old = GetWindowTextLengthW(e);
    }
    wchar_t* cur = NULL;
    if (old > 0) {
        cur = (wchar_t*)xalloc(((size_t)old + 1) * sizeof(wchar_t));
        GetWindowTextW(e, cur, old + 1);
    }
    wsb b; wsb_init(&b);
    if (cur) { wsb_add(&b, cur); free(cur); }
    wsb_addf(&b, L"[%s] %s\r\n", st, s);
    SetWindowTextW(e, b.b);
    int len = GetWindowTextLengthW(e);
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_SCROLLCARET, 0, 0);
    wsb_free(&b);
}

static DWORD WINAPI dl_thread_manifest(void* p) {
    dl_ctx* c = (dl_ctx*)p;
    c->list = (dl_list*)xalloc(sizeof(dl_list));
    memset(c->list, 0, sizeof(dl_list));
    wchar_t err[640] = L"";
    if (!dl_fetch_manifest(c->list, err, 640)) {
        free(c->list); c->list = NULL;
        PostMessageW(g_dlwin, WM_APP_DL_READY, 0, (LPARAM)wdup(err));
    } else {
        PostMessageW(g_dlwin, WM_APP_DL_READY, (WPARAM)c->list, 0);
    }
    free(c);
    return 0;
}

static DWORD WINAPI dl_thread_install(void* p) {
    dl_ctx* c = (dl_ctx*)p;
    /* 加载器：自动补原版 + 取最新稳定加载器 + 安装（等价于一键流程） */
    if (c->kind >= LD_FABRIC && c->kind <= LD_NEOFORGE) {
        if (!ld_version_installed(c->gamedir, c->ver)) {
            if (c->prog->cur[0]) c->prog->cur[0] = 0;
            if (!dl_install_version(c->gamedir, c->ver, NULL, 0, c->prog, c->err, 640)) {
                if (!c->err[0]) wcsncpy_s(c->err, 640, L"自动下载原版失败", _TRUNCATE);
                PostMessageW(g_dlwin, WM_APP_DL_DONE, 0, (LPARAM)c);
                return 0;
            }
        }
        /* 取最新稳定加载器版本 */
        ld_entry* ent = NULL; int n = 0; wchar_t le[320] = L"";
        if (!loader_list((ld_kind)c->kind, c->ver, &ent, &n, le, 320) || n <= 0) {
            if (!c->err[0]) _snwprintf_s(c->err, 640, _TRUNCATE, L"获取 %s 版本列表失败", loader_kind_name((ld_kind)c->kind));
            loader_list_free(ent, n);
            PostMessageW(g_dlwin, WM_APP_DL_DONE, 0, (LPARAM)c);
            return 0;
        }
        int chosen = -1;
        for (int i = 0; i < n; i++) if (ent[i].stable) { chosen = i; break; }
        if (chosen < 0) chosen = 0;
        wchar_t* loader = wdup(ent[chosen].version);
        loader_list_free(ent, n);
        if (!loader) {
            wcsncpy_s(c->err, 640, L"未找到可用加载器版本", _TRUNCATE);
            PostMessageW(g_dlwin, WM_APP_DL_DONE, 0, (LPARAM)c);
            return 0;
        }
        if (c->prog->cur[0]) c->prog->cur[0] = 0;
        wcsncpy_s(c->prog->cur, 256, loader_kind_name((ld_kind)c->kind), _TRUNCATE);
        int ok = loader_install((ld_kind)c->kind, c->gamedir, c->ver, c->ver, loader,
                                c->prog, c->err, 640);
        free(loader);
        PostMessageW(g_dlwin, WM_APP_DL_DONE, ok ? 1 : 0, (LPARAM)c);
        return 0;
    }
    /* 原版 */
    int ok = dl_install_version(c->gamedir, c->ver, c->custom, 0, c->prog, c->err, 640);
    PostMessageW(g_dlwin, WM_APP_DL_DONE, ok ? 1 : 0, (LPARAM)c);
    return 0;
}

static void dl_start_install(HWND d) {
    wchar_t ver[256];
    HWND cb = GetDlgItem(d, IDC_D_VER);
    int idx = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (idx < 0) {
        SetWindowTextW(GetDlgItem(d, IDC_D_PROG), L"请先选择要下载的版本");
        return;
    }
    SendMessageW(cb, CB_GETLBTEXT, (WPARAM)idx, (LPARAM)ver);
    if (!g_cfg.game_dir) { SetWindowTextW(GetDlgItem(d, IDC_D_PROG), L"未设置游戏目录"); return; }
    int type = (int)SendMessageW(GetDlgItem(d, IDC_D_TYPE), CB_GETCURSEL, 0, 0);
    if (type < 0) type = 0;
    int wk = -1;
    if (type == 1) {
        /* 加载器类型下拉：Fabric/Forge/NeoForge/Quilt（顺序与 ld_kind 一致） */
        int lk = (int)SendMessageW(GetDlgItem(d, IDC_D_LOADERKIND), CB_GETCURSEL, 0, 0);
        if (lk < 0) lk = 0;
        wk = lk;
    }
    wchar_t cust[256];
    GetWindowTextW(GetDlgItem(d, IDC_D_CUSTOM), cust, 256);
    if (type == 0 && cust[0]) {
        const wchar_t* bad = L"\\/:*?\"<>|";
        int badc = 0;
        for (wchar_t* p = cust; *p && !badc; p++) {
            if (*p < 0x20) badc = 1;
            for (const wchar_t* q = bad; *q; q++) if (*p == *q) { badc = 1; break; }
        }
        if (badc) {
            SetWindowTextW(GetDlgItem(d, IDC_D_PROG), L"自定义版本名不能含 \\ / : * ? \" < > | 等字符");
            return;
        }
    }
    dl_ctx* c = (dl_ctx*)xalloc(sizeof(dl_ctx));
    memset(c, 0, sizeof(*c));
    c->gamedir = wdup(g_cfg.game_dir);
    c->ver = wdup(ver);
    /* 原版可自定义名；加载器安装到原版名（dir_id == ver），忽略自定义名 */
    c->custom = (type == 0) ? wdup(cust) : NULL;
    c->kind = wk;   /* 原版 = -1；加载器 = ld_kind */
    c->prog = (dl_progress*)xalloc(sizeof(dl_progress));
    memset(c->prog, 0, sizeof(dl_progress));
    SetWindowLongPtrW(d, GWLP_USERDATA, (LONG_PTR)c);
    g_dl_installing = 1;
    EnableWindow(GetDlgItem(d, IDC_D_OK), FALSE);
    EnableWindow(GetDlgItem(d, IDC_D_CLOSE), FALSE);
    EnableWindow(GetDlgItem(d, IDC_D_VER), FALSE);
    EnableWindow(GetDlgItem(d, IDC_D_CUSTOM), FALSE);
    SetWindowTextW(GetDlgItem(d, IDC_D_PROG), L"准备下载…");
    {
        wchar_t line[256];
        wchar_t who[64];
        if (wk < 0)
            wcsncpy_s(who, 64, cust[0] ? L"，自定义版本名" : L"（原版）", _TRUNCATE);
        else
            _snwprintf_s(who, 64, _TRUNCATE, L" + %s", loader_kind_name((ld_kind)wk));
        _snwprintf_s(line, 256, _TRUNCATE,
                     L"开始下载%s（%d 线程并发，BMCLAPI 镜像优先）",
                     who, g_cfg.dl_threads);
        dlog_append(line);
    }
    HWND bar = GetDlgItem(d, IDC_D_BAR);
    SendMessageW(bar, PBM_SETRANGE32, 0, 100);
    SendMessageW(bar, PBM_SETPOS, 0, 0);
    SetTimer(d, IDT_DL, 200, NULL);   /* 200ms 轮询后台进度，界面实时刷新 */
    HANDLE t = CreateThread(NULL, 65536, dl_thread_install, c, 0, NULL);
    if (t) CloseHandle(t);
}

static void open_loader(void);   /* 前向声明：dl_proc 中按钮打开嵌套的加载器对话框 */
static void open_modpack(void);  /* 前向声明：内容中心里安装本地整合包 */

static LRESULT CALLBACK dl_proc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_dlwin = d;   /* 必须在启动后台线程前赋值，线程要 PostMessage 到本窗口 */
        /* PCL 风格：类型（原版/加载器）→ 加载器类型 → 版本下拉 → 一键下载 */
        mk(0, L"STATIC", L"类型", SS_LEFT, 14, 10, 40, 16, d, -1);
        HWND type = mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            58, 8, 120, 200, d, IDC_D_TYPE);
        SendMessageW(type, CB_ADDSTRING, 0, (LPARAM)L"原版");
        SendMessageW(type, CB_ADDSTRING, 0, (LPARAM)L"模组加载器");
        SendMessageW(type, CB_SETCURSEL, 0, 0);
        mk(0, L"STATIC", L"加载器", SS_LEFT, 190, 10, 40, 16, d, -1);
        HWND lk = mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            234, 8, 130, 200, d, IDC_D_LOADERKIND);
        SendMessageW(lk, CB_ADDSTRING, 0, (LPARAM)L"Fabric");
        SendMessageW(lk, CB_ADDSTRING, 0, (LPARAM)L"Forge");
        SendMessageW(lk, CB_ADDSTRING, 0, (LPARAM)L"NeoForge");
        SendMessageW(lk, CB_ADDSTRING, 0, (LPARAM)L"Quilt");
        SendMessageW(lk, CB_SETCURSEL, 0, 0);
        EnableWindow(lk, FALSE);   /* 原版下禁用加载器类型 */
        mk(0, L"STATIC", L"版本（BMCLAPI 镜像，点“下载”安装；资源与对应 Java 自动补齐）",
           SS_LEFT, 14, 34, 432, 16, d, -1);
        mk(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
           WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_SORT,
           14, 52, 432, 200, d, IDC_D_VER);
        mk(0, L"STATIC", L"自定义版本名（可选，仅原版；留空用版本号）",
           SS_LEFT, 14, 78, 432, 14, d, -1);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
           14, 94, 432, 24, d, IDC_D_CUSTOM);
        mk(0, L"BUTTON", L"下载", WS_TABSTOP | BS_PUSHBUTTON, 14, 126, 100, 30, d, IDC_D_OK);
        mk(0, L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON, 120, 126, 100, 30, d, IDC_D_CLOSE);
        mk(0, L"STATIC", L"", SS_LEFT, 14, 162, 432, 20, d, IDC_D_STATUS);
        mk(0, L"STATIC", L"正在拉取版本清单…", SS_LEFT, 14, 184, 432, 20, d, IDC_D_PROG);
        mk(0, PROGRESS_CLASSW, L"", 0, 14, 208, 432, 18, d, IDC_D_BAR);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
           14, 232, 432, 96, d, IDC_D_LOG);
        SetWindowTextW(GetDlgItem(d, IDC_D_STATUS), L"进度 0%");
        dl_ctx* c = (dl_ctx*)xalloc(sizeof(dl_ctx));
        memset(c, 0, sizeof(*c));
        g_dl_installing = 0;
        HANDLE t = CreateThread(NULL, 65536, dl_thread_manifest, c, 0, NULL);
        if (t) CloseHandle(t);
        return 0;
    }
    case WM_TIMER: {
        if (w != IDT_DL) break;
        dl_ctx* c = (dl_ctx*)GetWindowLongPtrW(d, GWLP_USERDATA);
        if (!c || !c->prog) break;
        dl_progress* p = c->prog;
        LONG done = InterlockedCompareExchange(&p->files_done, 0, 0);
        LONG total = InterlockedCompareExchange(&p->files_total, 0, 0);
        LONG active = InterlockedCompareExchange(&p->active, 0, 0);
        LONG failed = InterlockedCompareExchange(&p->failed, 0, 0);
        LONG64 bytes = InterlockedCompareExchange64(&p->bytes_done, 0, 0);
        int pct = total > 0 ? (int)(done * 100 / total) : 0;
        wchar_t t[320];
        wchar_t* mb = fmt_bytes((uint64_t)bytes);
        _snwprintf_s(t, 320, _TRUNCATE, L"进度 %d%%  文件 %ld/%ld  并发 %ld  已下载 %s  失败 %ld",
                     pct, done, total, active, mb, failed);
        free(mb);
        SetWindowTextW(GetDlgItem(d, IDC_D_STATUS), t);
        SendMessageW(GetDlgItem(d, IDC_D_BAR), PBM_SETPOS, (WPARAM)pct, 0);
        wchar_t t2[320];
        _snwprintf_s(t2, 320, _TRUNCATE, L"%s%s", p->cur, active > 0 ? L" …" : L"");
        SetWindowTextW(GetDlgItem(d, IDC_D_PROG), t2);
        /* 每 ~10 个 tick（2 秒）向日志追加一行进度 */
        static int tick = 0;
        if (++tick % 10 == 0) {
            wchar_t* mb2 = fmt_bytes((uint64_t)bytes);
            wchar_t line[320];
            _snwprintf_s(line, 320, _TRUNCATE, L"文件 %ld/%ld（%d%%），已下载 %s", done, total, pct, mb2);
            free(mb2);
            dlog_append(line);
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_D_OK) { dl_start_install(d); return 0; }
        if (id == IDC_D_CLOSE) { modal_close(); return 0; }
        /* 类型切换：加载器时启用加载器类型下拉 */
        if (id == IDC_D_TYPE && HIWORD(w) == CBN_SELCHANGE) {
            int t = (int)SendMessageW(GetDlgItem(d, IDC_D_TYPE), CB_GETCURSEL, 0, 0);
            EnableWindow(GetDlgItem(d, IDC_D_LOADERKIND), t == 1);
            return 0;
        }
        return 0;
    }
    case WM_APP_DL_READY: {
        if (w) {
            if (g_dl_manifest) dl_list_free(g_dl_manifest);
            g_dl_manifest = (dl_list*)w;
            HWND cb = GetDlgItem(d, IDC_D_VER);
            SendMessageW(cb, CB_RESETCONTENT, 0, 0);
            for (int i = 0; i < g_dl_manifest->n; i++)
                SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)g_dl_manifest->v[i].id);
            SendMessageW(cb, CB_SETCURSEL, 0, 0);
            SetWindowTextW(GetDlgItem(d, IDC_D_PROG), L"清单已加载，选择版本后点“下载”");
        } else {
            wchar_t* err = (wchar_t*)l;
            SetWindowTextW(GetDlgItem(d, IDC_D_PROG), err ? err : L"获取版本清单失败");
            dlog_append(err ? err : L"获取版本清单失败");
            free(err);
        }
        return 0;
    }
    case WM_APP_DL_DONE: {
        dl_ctx* c = (dl_ctx*)l;
        g_dl_installing = 0;
        KillTimer(d, IDT_DL);
        EnableWindow(GetDlgItem(d, IDC_D_OK), TRUE);
        EnableWindow(GetDlgItem(d, IDC_D_CLOSE), TRUE);
        EnableWindow(GetDlgItem(d, IDC_D_VER), TRUE);
        EnableWindow(GetDlgItem(d, IDC_D_CUSTOM), TRUE);
        if (c->prog) {
            LONG done = c->prog->files_done, total = c->prog->files_total;
            SendMessageW(GetDlgItem(d, IDC_D_BAR), PBM_SETPOS, w ? 100 : 0, 0);
            if (w) {
                wchar_t* mb = fmt_bytes((uint64_t)c->prog->bytes_done);
                wchar_t t[320];
                const wchar_t* shown = (c->custom && c->custom[0]) ? c->custom : c->ver;
                _snwprintf_s(t, 320, _TRUNCATE, L"完成: %s 安装成功（文件 %ld/%ld，共下载 %s）。可关闭本窗口。",
                             shown, done, total, mb);
                free(mb);
                SetWindowTextW(GetDlgItem(d, IDC_D_STATUS), L"进度 100%");
                SetWindowTextW(GetDlgItem(d, IDC_D_PROG), t);
                dlog_append(t);
                ui_log(L"下载完成: %s（文件 %ld/%ld）", shown, done, total);
                rescan_versions();
            } else {
                SetWindowTextW(GetDlgItem(d, IDC_D_PROG), c->err[0] ? c->err : L"下载失败");
                dlog_append(c->err[0] ? c->err : L"下载失败");
                ui_log(L"下载失败: %s", c->err[0] ? c->err : L"未知错误");
            }
        }
        SetWindowLongPtrW(d, GWLP_USERDATA, 0);
        free(c->prog);
        free(c->gamedir);
        free(c->ver);
        free(c->custom);
        free(c);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(d, IDT_DL);
        return 0;
    case WM_CLOSE:
        if (g_dl_installing) {
            SetWindowTextW(GetDlgItem(d, IDC_D_PROG), L"正在下载/安装，完成前请不要关闭窗口");
            return 0;
        }
        modal_close();
        return 0;
    }
    return DefWindowProcW(d, msg, w, l);
}

static void open_download(void) {
    if (g_dlwin) { SetForegroundWindow(g_dlwin); return; }
    g_dlwin = modal_create(L"LLclDownload", L"下载与安装（游戏 / 模组加载器）", 470, 360);
    modal_run(g_dlwin);
    g_dlwin = NULL;
    if (g_dl_manifest) { dl_list_free(g_dl_manifest); g_dl_manifest = NULL; }
}

/* ================= 模组加载器（Fabric/Quilt） ================= */
static HWND g_ldwin = NULL;
static ld_entry* g_ld_entries = NULL;
static int g_ld_n = 0;
static int g_ld_busy = 0;

typedef struct { int ok; int kind; int n; void* data; wchar_t err[320]; } ld_jobres;

/* 判断某游戏版本是否已安装完整（存在 <gamedir>\versions\<id> 且含 json+jar）。
   加载器 profile 安装到自定义版本时，其 "id" 会被改写，因此这里按实际 GameDir 判定。 */
static int ld_version_installed(const wchar_t* gamedir, const wchar_t* id) {
    if (!gamedir || !*gamedir || !id || !*id) return 0;
    wchar_t* vdir = path_join3(gamedir, L"versions", id);
    wchar_t* jp = path_join(vdir, wcat3(id, L".jar", L""));
    int ok = dir_exists(vdir) && file_exists(jp);
    free(jp); free(vdir);
    return ok;
}

/* 判断是否为已安装的加载器版本（排除，避免给加载器再装加载器） */
static int ld_is_loader_version(const wchar_t* id) {
    return wcsstr(id, L"fabric") || wcsstr(id, L"quilt") ||
           wcsstr(id, L"forge") || wcsstr(id, L"neoforge");
}
/* 填充已下载的游戏版本（排除已安装的加载器版本） */
static void ld_fill_mcvers(HWND d) {
    HWND cb = GetDlgItem(d, IDC_LD_MCVER);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_nvers; i++) {
        const wchar_t* id = g_vers[i].id;
        if (ld_is_loader_version(id)) continue;
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)id);
    }
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
}
static wchar_t* ld_get_mcver(HWND d) {
    HWND cb = GetDlgItem(d, IDC_LD_MCVER);
    /* 可编辑下拉：优先取选中的条目文本；无选中则取编辑框内容（允许输入未安装的版本） */
    int ci = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    wchar_t t[128];
    if (ci >= 0) {
        SendMessageW(cb, CB_GETLBTEXT, ci, (LPARAM)t);
    } else {
        GetWindowTextW(cb, t, 128);
    }
    return wdup(t);
}
/* 读取某版本 JSON 里的 llcl_base_version（自定义版本记录的原版版本号）；无则返回自身 */
static wchar_t* ld_resolve_base(const wchar_t* display_id) {
    for (int i = 0; i < g_nvers; i++) {
        if (weq(g_vers[i].id, display_id)) {
            char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
            if (json_load_file(g_vers[i].json_path, &js, &t, &nt) && js) {
                const jsmntok_t* b = jm(js, t, "llcl_base_version");
                if (b && b->type == JSMN_STRING) {
                    wchar_t* bv = jwstrdup(js, b);
                    json_free(js, t);
                    if (bv) return bv;
                    free(bv);
                }
                json_free(js, t);
            }
            break;
        }
    }
    return wdup(display_id);
}
static ld_kind ld_get_kind(HWND d) {
    int ci = (int)SendMessageW(GetDlgItem(d, IDC_LD_KIND), CB_GETCURSEL, 0, 0);
    if (ci < 0) ci = 0;
    return (ld_kind)ci;   /* 顺序与下拉一致：Fabric/Quilt/Forge/NeoForge */
}

typedef struct { int kind; wchar_t* mcver; } ld_farg;
static DWORD WINAPI ld_fetch_thread(LPVOID p) {
    ld_farg* a = (ld_farg*)p;
    ld_jobres* r = (ld_jobres*)xalloc(sizeof(ld_jobres));
    memset(r, 0, sizeof(*r));
    r->kind = 0;
    r->ok = loader_list((ld_kind)a->kind, a->mcver, (ld_entry**)&r->data, &r->n, r->err, 320);
    free(a->mcver); free(a);
    PostMessageW(g_ldwin, WM_APP_LD_DONE, (WPARAM)r, 0);
    return 0;
}
typedef struct { int kind; wchar_t* mcver; wchar_t* dir_id; int auto_base; wchar_t* loader; } ld_iarg;
static DWORD WINAPI ld_install_thread(LPVOID p) {
    ld_iarg* a = (ld_iarg*)p;
    ld_jobres* r = (ld_jobres*)xalloc(sizeof(ld_jobres));
    memset(r, 0, sizeof(*r));
    r->kind = 1;
    dl_progress prog; memset(&prog, 0, sizeof(prog));
    /* 一键流程：未选具体加载器版本时，先取最新稳定版 */
    if (!a->loader) {
        ld_entry* ent = NULL; int n = 0; wchar_t le[320] = L"";
        if (!loader_list((ld_kind)a->kind, a->mcver, &ent, &n, le, 320) || n <= 0) {
            _snwprintf_s(r->err, 320, _TRUNCATE, L"获取 %s 版本列表失败", loader_kind_name((ld_kind)a->kind));
            r->ok = 0;
            loader_list_free(ent, n);
            free(a->mcver); free(a->dir_id); free(a->loader); free(a);
            PostMessageW(g_ldwin, WM_APP_LD_DONE, (WPARAM)r, 1);
            return 0;
        }
        /* 优先选最新稳定版 */
        int chosen = -1;
        for (int i = 0; i < n; i++) if (ent[i].stable) { chosen = i; break; }
        if (chosen < 0) chosen = 0;
        a->loader = wdup(ent[chosen].version);
        loader_list_free(ent, n);
        if (!a->loader) {
            _snwprintf_s(r->err, 320, _TRUNCATE, L"未找到可用的加载器版本");
            r->ok = 0;
            free(a->mcver); free(a->dir_id); free(a); 
            PostMessageW(g_ldwin, WM_APP_LD_DONE, (WPARAM)r, 1);
            return 0;
        }
    }
    /* 一键流程：原版尚未安装时先自动下载（安装到实际用到的基础版本号），
     * 再把加载器装上去。dir_id == base 时即安装到原版版本目录。 */
    if (a->auto_base && !ld_version_installed(g_cfg.game_dir, a->dir_id ? a->dir_id : a->mcver)) {
        r->err[0] = 0;
        if (prog.cur[0]) prog.cur[0] = 0;
        if (!dl_install_version(g_cfg.game_dir, a->mcver, NULL, 0, &prog, r->err, 320)) {
            if (!r->err[0]) wcsncpy_s(r->err, 320, L"自动下载原版失败", _TRUNCATE);
            r->ok = 0;
            free(a->mcver); free(a->dir_id); free(a->loader); free(a);
            PostMessageW(g_ldwin, WM_APP_LD_DONE, (WPARAM)r, 1);
            return 0;
        }
    }
    r->ok = loader_install((ld_kind)a->kind, g_cfg.game_dir, a->mcver, a->dir_id,
                           a->loader, &prog, r->err, 320);
    free(a->mcver); free(a->dir_id); free(a->loader); free(a);
    PostMessageW(g_ldwin, WM_APP_LD_DONE, (WPARAM)r, 1);
    return 0;
}

static void ld_do_fetch(HWND d) {
    if (g_ld_busy) return;
    wchar_t* disp = ld_get_mcver(d);
    if (!disp[0]) { free(disp); SetWindowTextW(GetDlgItem(d, IDC_LD_STATUS), L"请先选择/输入游戏版本"); return; }
    /* 自定义版本：用其记录的原版版本号查询（直接用自定义名会 404） */
    wchar_t* base = ld_resolve_base(disp);
    free(disp);
    ld_farg* a = (ld_farg*)xalloc(sizeof(ld_farg));
    a->kind = (int)ld_get_kind(d); a->mcver = base;
    g_ld_busy = 1;
    SetWindowTextW(GetDlgItem(d, IDC_LD_STATUS), L"正在获取加载器版本…");
    HANDLE t = CreateThread(NULL, 65536, ld_fetch_thread, a, 0, NULL);
    if (t) CloseHandle(t);
}
static void ld_do_install(HWND d) {
    if (g_ld_busy) return;
    /* 选中了列表项：安装该版本；否则一键安装最新稳定版 */
    int sel = (int)SendMessageW(GetDlgItem(d, IDC_LD_LIST), LB_GETCURSEL, 0, 0);
    wchar_t* disp = ld_get_mcver(d);
    if (!disp[0]) { free(disp); SetWindowTextW(GetDlgItem(d, IDC_LD_STATUS), L"请先选择/输入游戏版本"); return; }
    wchar_t* base = ld_resolve_base(disp);   /* 查询用原版版本号 */
    ld_iarg* a = (ld_iarg*)xalloc(sizeof(ld_iarg));
    a->kind = (int)ld_get_kind(d);
    a->mcver = base;
    a->dir_id = disp;                        /* 实际游戏目录名 */
    a->loader = NULL;
    a->auto_base = 1;
    if (sel >= 0 && g_ld_entries && sel < g_ld_n) {
        a->loader = wdup(g_ld_entries[sel].version);
    }
    g_ld_busy = 1;
    SetWindowTextW(GetDlgItem(d, IDC_LD_STATUS),
                   a->loader ? L"正在安装加载器…" : L"正在下载原版并安装最新加载器…");
    SendMessageW(GetDlgItem(d, IDC_LD_BAR), PBM_SETPOS, 5, 0);
    HANDLE t = CreateThread(NULL, 65536, ld_install_thread, a, 0, NULL);
    if (t) CloseHandle(t);
}

static LRESULT CALLBACK ld_proc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_ldwin = d;
        mk(0, L"STATIC", L"游戏版本", SS_LEFT, 12, 8, 80, 16, d, -1);
        mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL,
           12, 26, 180, 300, d, IDC_LD_MCVER);
        mk(0, L"STATIC", L"加载器", SS_LEFT, 204, 8, 80, 16, d, -1);
        HWND kd = mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                     204, 26, 120, 200, d, IDC_LD_KIND);
        SendMessageW(kd, CB_ADDSTRING, 0, (LPARAM)L"Fabric");
        SendMessageW(kd, CB_ADDSTRING, 0, (LPARAM)L"Quilt");
        SendMessageW(kd, CB_ADDSTRING, 0, (LPARAM)L"Forge");
        SendMessageW(kd, CB_ADDSTRING, 0, (LPARAM)L"NeoForge");
        SendMessageW(kd, CB_SETCURSEL, 0, 0);
        mk(0, L"BUTTON", L"获取版本", WS_TABSTOP | BS_PUSHBUTTON,
           332, 25, 110, 24, d, IDC_LD_FETCH);
        ld_fill_mcvers(d);
        mk(0, L"STATIC", L"加载器版本（可留空，点“一键安装”自动用最新稳定版）", SS_LEFT, 12, 56, 430, 16, d, -1);
        mk(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_TABSTOP,
           12, 74, 430, 220, d, IDC_LD_LIST);
        mk(0, L"BUTTON", L"一键安装", WS_TABSTOP | BS_PUSHBUTTON,
           12, 302, 150, 28, d, IDC_LD_INSTALL);
        mk(0, PROGRESS_CLASSW, L"", 0, 170, 306, 180, 16, d, IDC_LD_BAR);
        SendMessageW(GetDlgItem(d, IDC_LD_BAR), PBM_SETRANGE32, 0, 100);
        mk(0, L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON,
           360, 302, 82, 28, d, IDC_LD_CLOSE);
        mk(0, L"STATIC", L"选游戏版本与加载器类型，点“一键安装”：原版未装会自动下载，加载器取最新稳定版。",
           SS_LEFT, 12, 334, 430, 18, d, IDC_LD_STATUS);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_LD_FETCH) { ld_do_fetch(d); return 0; }
        if (id == IDC_LD_INSTALL) { ld_do_install(d); return 0; }
        if (id == IDC_LD_CLOSE) { modal_close(); return 0; }
        if (id == IDC_LD_LIST && HIWORD(w) == LBN_DBLCLK) { ld_do_install(d); return 0; }
        return 0;
    }
    case WM_APP_LD_DONE: {
        ld_jobres* r = (ld_jobres*)w;
        int kind = (int)l;
        g_ld_busy = 0;
        if (kind == 0) {
            loader_list_free(g_ld_entries, g_ld_n);
            g_ld_entries = (ld_entry*)r->data; g_ld_n = r->n;
            HWND lb = GetDlgItem(d, IDC_LD_LIST);
            SendMessageW(lb, LB_RESETCONTENT, 0, 0);
            for (int i = 0; i < g_ld_n; i++) {
                wchar_t t[128];
                _snwprintf_s(t, 128, _TRUNCATE, L"%s%s", g_ld_entries[i].version,
                             g_ld_entries[i].stable ? L"  (稳定)" : L"  (测试版)");
                SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t);
            }
            if (r->ok) {
                wchar_t t[128];
                _snwprintf_s(t, 128, _TRUNCATE, L"找到 %d 个加载器版本，选择后点安装", g_ld_n);
                SetWindowTextW(GetDlgItem(d, IDC_LD_STATUS), t);
            } else {
                SetWindowTextW(GetDlgItem(d, IDC_LD_STATUS), r->err[0] ? r->err : L"获取失败");
            }
        } else {
            SendMessageW(GetDlgItem(d, IDC_LD_BAR), PBM_SETPOS, r->ok ? 100 : 0, 0);
            if (r->ok) {
                SetWindowTextW(GetDlgItem(d, IDC_LD_STATUS), L"加载器安装成功，可关闭后在版本列表选择并启动");
                ui_log(L"模组加载器安装完成");
            } else {
                SetWindowTextW(GetDlgItem(d, IDC_LD_STATUS), r->err[0] ? r->err : L"安装失败");
            }
        }
        free(r);
        mem_trim();
        return 0;
    }
    case WM_CLOSE:
        if (g_ld_busy) {
            SetWindowTextW(GetDlgItem(d, IDC_LD_STATUS), L"正在安装加载器，完成前请不要关闭窗口");
            return 0;
        }
        modal_close();
        return 0;
    }
    return DefWindowProcW(d, msg, w, l);
}

static void open_loader(void) {
    if (g_ldwin) { SetForegroundWindow(g_ldwin); return; }
    g_ldwin = modal_create(L"LLclLoader", L"安装模组加载器", 460, 375);
    modal_run(g_ldwin);
    g_ldwin = NULL;
    loader_list_free(g_ld_entries, g_ld_n); g_ld_entries = NULL; g_ld_n = 0;
    g_ld_busy = 0;
    rescan_versions();   /* 关闭后刷新主版本列表（新增 fabric/quilt 版本） */
}

/* ================= 下载中心（Mod/光影/资源包） ================= */
static HWND g_ctwin = NULL;
static ct_proj* g_ct_projs = NULL;
static int g_ct_n = 0;
static ct_ver* g_ct_vers = NULL;
static int g_ct_vn = 0;
static int g_ct_busy = 0;

typedef struct { int ok; int n; void* data; wchar_t err[320]; } ct_jobres;

static ct_type ct_get_type(HWND d) {
    int i = (int)SendMessageW(GetDlgItem(d, IDC_CT_TYPE), CB_GETCURSEL, 0, 0);
    if (i < 0 || i > 3) i = 0;
    return (ct_type)i;
}
static void ct_fill_loaders(HWND d, ct_type t) {
    HWND ld = GetDlgItem(d, IDC_CT_LOADER);
    SendMessageW(ld, CB_RESETCONTENT, 0, 0);
    const wchar_t* items[8]; int n = 0;
    if (t == CT_MOD) {
        items[n++] = L"自动"; items[n++] = L"Fabric"; items[n++] = L"Forge";
        items[n++] = L"NeoForge"; items[n++] = L"Quilt";
    } else if (t == CT_SHADER) {
        items[n++] = L"自动"; items[n++] = L"Iris"; items[n++] = L"OptiFine";
    } else {
        items[n++] = L"自动";
    }
    for (int i = 0; i < n; i++)
        SendMessageW(ld, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageW(ld, CB_SETCURSEL, 0, 0);
}
static wchar_t* ct_get_loader(HWND d) {
    HWND ld = GetDlgItem(d, IDC_CT_LOADER);
    int ci = (int)SendMessageW(ld, CB_GETCURSEL, 0, 0);
    if (ci <= 0) return wdup(L"");
    wchar_t txt[64];
    SendMessageW(ld, CB_GETLBTEXT, ci, (LPARAM)txt);
    for (wchar_t* p = txt; *p; p++) if (*p >= L'A' && *p <= L'Z') *p += 32;
    return wdup(txt);
}
static void ct_fill_mcvers(HWND d) {
    HWND mv = GetDlgItem(d, IDC_CT_MCVER);
    SendMessageW(mv, CB_RESETCONTENT, 0, 0);
    SendMessageW(mv, CB_ADDSTRING, 0, (LPARAM)L"全部版本");
    for (int i = 0; i < g_nvers; i++)
        SendMessageW(mv, CB_ADDSTRING, 0, (LPARAM)g_vers[i].id);
    SendMessageW(mv, CB_SETCURSEL, 0, 0);
}
static wchar_t* ct_get_mcver(HWND d) {
    HWND mv = GetDlgItem(d, IDC_CT_MCVER);
    int ci = (int)SendMessageW(mv, CB_GETCURSEL, 0, 0);
    if (ci <= 0) return wdup(L"");
    wchar_t txt[128];
    SendMessageW(mv, CB_GETLBTEXT, ci, (LPARAM)txt);
    return wdup(txt);
}

/* 填充"安装到"目标：共享目录 + 每个已安装版本 */
static void ct_fill_targets(HWND d) {
    HWND tg = GetDlgItem(d, IDC_CT_TARGET);
    SendMessageW(tg, CB_RESETCONTENT, 0, 0);
    SendMessageW(tg, CB_ADDSTRING, 0, (LPARAM)L"共享目录（所有版本共用）");
    for (int i = 0; i < g_nvers; i++)
        SendMessageW(tg, CB_ADDSTRING, 0, (LPARAM)g_vers[i].id);
    SendMessageW(tg, CB_SETCURSEL, 0, 0);
}
/* 取"安装到"目标版本 id；选共享目录（索引0）返回 NULL */
static wchar_t* ct_get_target(HWND d) {
    HWND tg = GetDlgItem(d, IDC_CT_TARGET);
    int ci = (int)SendMessageW(tg, CB_GETCURSEL, 0, 0);
    if (ci <= 0) return NULL;
    wchar_t txt[128];
    SendMessageW(tg, CB_GETLBTEXT, ci, (LPARAM)txt);
    return wdup(txt);
}
/* 计算内容文件的目标目录：
   - 共享 / 非隔离：<gamedir>\<sub>
   - 选了具体版本且开启版本隔离：<gamedir>\versions\<target>\instance\<sub> */
static wchar_t* content_target_dir(const wchar_t* gamedir, const wchar_t* target, const wchar_t* sub) {
    if (target && target[0] && g_cfg.version_isolate) {
        wchar_t* a = path_join3(gamedir, L"versions", target);
        wchar_t* b = path_join(a, L"instance");
        wchar_t* c = path_join(b, sub);
        free(a); free(b);
        return c;
    }
    return path_join(gamedir, sub);
}

typedef struct { wchar_t* query; ct_type type; wchar_t* mcver; wchar_t* loader; } ct_sarg;
static DWORD WINAPI ct_search_thread(LPVOID p) {
    ct_sarg* a = (ct_sarg*)p;
    ct_jobres* r = (ct_jobres*)xalloc(sizeof(ct_jobres));
    memset(r, 0, sizeof(*r));
    r->ok = ct_search(a->query, a->type, a->mcver, a->loader,
                      (ct_proj**)&r->data, &r->n, r->err, 320);
    free(a->query); free(a->mcver); free(a->loader); free(a);
    PostMessageW(g_ctwin, WM_APP_CT_DONE, (WPARAM)r, 0);
    return 0;
}
typedef struct { wchar_t* pid; wchar_t* mcver; wchar_t* loader; } ct_varg;
static DWORD WINAPI ct_ver_thread(LPVOID p) {
    ct_varg* a = (ct_varg*)p;
    ct_jobres* r = (ct_jobres*)xalloc(sizeof(ct_jobres));
    memset(r, 0, sizeof(*r));
    r->ok = ct_versions(a->pid, a->mcver, a->loader,
                        (ct_ver**)&r->data, &r->n, r->err, 320);
    free(a->pid); free(a->mcver); free(a->loader); free(a);
    PostMessageW(g_ctwin, WM_APP_CT_DONE, (WPARAM)r, 1);
    return 0;
}
typedef struct { ct_ver v; wchar_t* dest; ct_type type; wchar_t* gamedir; } ct_darg;
static DWORD WINAPI ct_dl_thread(LPVOID p) {
    ct_darg* a = (ct_darg*)p;
    ct_jobres* r = (ct_jobres*)xalloc(sizeof(ct_jobres));
    memset(r, 0, sizeof(*r));
    if (a->type == CT_MODPACK) {
        /* 整合包不是简单丢进 mods：先下载 .mrpack，再走 modpack_install
           （安装游戏版本、加载器、mods、overrides）。 */
        int dl = ct_download(&a->v, a->dest, NULL, r->err, 320);
        if (dl) {
            dl_progress prog;
            memset(&prog, 0, sizeof(prog));
            r->ok = modpack_install(a->gamedir, a->dest, g_cfg.version_isolate,
                                    &prog, r->err, 320);
            _wremove(a->dest);
        }
    } else {
        r->ok = ct_download(&a->v, a->dest, NULL, r->err, 320);
    }
    free(a->dest); free(a->gamedir);
    free(a->v.id); free(a->v.name); free(a->v.url); free(a->v.filename);
    free(a);
    PostMessageW(g_ctwin, WM_APP_CT_DONE, (WPARAM)r, 2);
    return 0;
}

static void ct_do_search(HWND d) {
    if (g_ct_busy) return;
    wchar_t q[256];
    GetWindowTextW(GetDlgItem(d, IDC_CT_QUERY), q, 256);
    ct_sarg* a = (ct_sarg*)xalloc(sizeof(ct_sarg));
    a->query = wdup(q);
    a->type = ct_get_type(d);
    a->mcver = ct_get_mcver(d);
    a->loader = ct_get_loader(d);
    SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), L"搜索中…");
    g_ct_busy = 1;
    HANDLE t = CreateThread(NULL, 65536, ct_search_thread, a, 0, NULL);
    if (t) CloseHandle(t);
}
static void ct_do_versions(HWND d) {
    if (g_ct_busy) return;
    int sel = (int)SendMessageW(GetDlgItem(d, IDC_CT_PLIST), LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || !g_ct_projs) {
        SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), L"请先在左侧选择一个项目");
        return;
    }
    ct_varg* a = (ct_varg*)xalloc(sizeof(ct_varg));
    a->pid = wdup(g_ct_projs[sel].id);
    a->mcver = ct_get_mcver(d);
    a->loader = ct_get_loader(d);
    SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), L"获取版本列表…");
    g_ct_busy = 1;
    HANDLE t = CreateThread(NULL, 65536, ct_ver_thread, a, 0, NULL);
    if (t) CloseHandle(t);
}
static void ct_do_download(HWND d) {
    if (g_ct_busy) return;
    int sel = (int)SendMessageW(GetDlgItem(d, IDC_CT_VLIST), LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || !g_ct_vers) {
        SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), L"请先在右侧选择一个版本");
        return;
    }
    ct_type t = ct_get_type(d);
    wchar_t* gd = wdup(g_cfg.game_dir ? g_cfg.game_dir : L".");
    wchar_t* dest = NULL;
    if (t == CT_MODPACK) {
        wchar_t* tmp = path_join(gd, L".llcl_tmp");
        ensure_dir_rec(tmp);
        dest = path_join(tmp, path_base(g_ct_vers[sel].filename));
        free(tmp);
        SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), L"正在下载并安装整合包（含游戏/加载器，可能需要数分钟）…");
    } else {
        const wchar_t* sub = (t == CT_MOD) ? L"mods" : (t == CT_SHADER) ? L"shaderpacks" : L"resourcepacks";
        wchar_t* target = ct_get_target(d);
        wchar_t* dir = content_target_dir(gd, target, sub);
        free(target);
        ensure_dir_rec(dir);
        dest = path_join(dir, path_base(g_ct_vers[sel].filename));
        free(dir);
        SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), L"下载中（详见主窗口日志）…");
    }
    ct_darg* a = (ct_darg*)xalloc(sizeof(ct_darg));
    a->v = g_ct_vers[sel];
    a->v.id = wdup(g_ct_vers[sel].id);
    a->v.name = wdup(g_ct_vers[sel].name);
    a->v.url = wdup(g_ct_vers[sel].url);
    a->v.filename = wdup(g_ct_vers[sel].filename);
    a->dest = dest;
    a->type = t;
    a->gamedir = gd;
    SendMessageW(GetDlgItem(d, IDC_CT_BAR), PBM_SETPOS, 10, 0);
    g_ct_busy = 1;
    HANDLE th = CreateThread(NULL, 65536, ct_dl_thread, a, 0, NULL);
    if (th) CloseHandle(th);
}

static LRESULT CALLBACK content_proc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        /* 第一行：类型 / 加载器 / 版本 / 搜索按钮 */
        mk(0, L"STATIC", L"类型", SS_LEFT, 10, 6, 40, 14, d, -1);
        HWND ty = mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
                     10, 22, 90, 200, d, IDC_CT_TYPE);
        SendMessageW(ty, CB_ADDSTRING, 0, (LPARAM)L"模组");
        SendMessageW(ty, CB_ADDSTRING, 0, (LPARAM)L"光影");
        SendMessageW(ty, CB_ADDSTRING, 0, (LPARAM)L"资源包");
        SendMessageW(ty, CB_ADDSTRING, 0, (LPARAM)L"整合包");
        SendMessageW(ty, CB_SETCURSEL, 0, 0);
        mk(0, L"STATIC", L"加载器", SS_LEFT, 108, 6, 50, 14, d, -1);
        mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
           108, 22, 110, 200, d, IDC_CT_LOADER);
        mk(0, L"STATIC", L"游戏版本", SS_LEFT, 226, 6, 60, 14, d, -1);
        mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST,
           226, 22, 140, 200, d, IDC_CT_MCVER);
        ct_fill_loaders(d, CT_MOD);
        ct_fill_mcvers(d);
        mk(0, L"BUTTON", L"搜索", WS_TABSTOP | BS_DEFPUSHBUTTON,
           374, 22, 160, 24, d, IDC_CT_SEARCH);
        /* 第二行：关键词 */
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
           10, 50, 524, 22, d, IDC_CT_QUERY);
        /* 列表 */
        mk(0, L"STATIC", L"项目（双击自动加载版本）", SS_LEFT, 10, 78, 260, 14, d, -1);
        mk(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY,
           10, 94, 266, 262, d, IDC_CT_PLIST);
        mk(0, L"STATIC", L"版本（双击自动下载）", SS_LEFT, 286, 78, 260, 14, d, -1);
        mk(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY,
           286, 94, 248, 262, d, IDC_CT_VLIST);
        /* 底部：安装到目标 + 下载 + 关闭 */
        mk(0, L"STATIC", L"安装到", SS_LEFT, 10, 368, 46, 14, d, -1);
        mk(0, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
           56, 364, 160, 200, d, IDC_CT_TARGET);
        ct_fill_targets(d);
        mk(0, L"BUTTON", L"下载选中版本", WS_TABSTOP | BS_PUSHBUTTON,
           224, 364, 120, 26, d, IDC_CT_DOWNLOAD);
        mk(0, L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON,
           450, 364, 90, 26, d, IDC_CT_CLOSE);
        mk(0, L"BUTTON", L"安装本地整合包", WS_TABSTOP | BS_PUSHBUTTON,
            350, 364, 94, 26, d, IDC_CT_LOCALMP);
        mk(0, PROGRESS_CLASSW, L"", 0, 10, 400, 530, 16, d, IDC_CT_BAR);
        SendMessageW(GetDlgItem(d, IDC_CT_BAR), PBM_SETRANGE32, 0, 100);
        mk(0, L"STATIC", L"输入关键词后点“搜索”；“安装到”可选择放进哪个游戏（开启版本隔离时按版本分开）。",
           SS_LEFT, 10, 422, 530, 18, d, IDC_CT_STATUS);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        int code = HIWORD(w);
        if (id == IDC_CT_SEARCH) { ct_do_search(d); return 0; }
        if (id == IDC_CT_DOWNLOAD) { ct_do_download(d); return 0; }
        if (id == IDC_CT_LOCALMP) { open_modpack(); return 0; }
        if (id == IDC_CT_CLOSE) { modal_close(); return 0; }
        if (id == IDC_CT_TYPE && code == CBN_SELCHANGE) {
            ct_type nt = ct_get_type(d);
            ct_fill_loaders(d, nt);
            /* 整合包自带安装目标（版本 instance），"安装到"只对 Mod/光影/资源包有意义 */
            EnableWindow(GetDlgItem(d, IDC_CT_TARGET), nt != CT_MODPACK);
            return 0;
        }
        if (id == IDC_CT_PLIST && code == LBN_DBLCLK) { ct_do_versions(d); return 0; }
        if (id == IDC_CT_VLIST && code == LBN_DBLCLK) { ct_do_download(d); return 0; }
        return 0;
    }
    case WM_APP_CT_DONE: {
        ct_jobres* r = (ct_jobres*)w;
        int kind = (int)l;
        g_ct_busy = 0;
        if (kind == 0) {
            ct_ver_free(g_ct_vers, g_ct_vn); g_ct_vers = NULL; g_ct_vn = 0;
            ct_proj_free(g_ct_projs, g_ct_n);
            g_ct_projs = (ct_proj*)r->data; g_ct_n = r->n;
            HWND lb = GetDlgItem(d, IDC_CT_PLIST);
            SendMessageW(lb, LB_RESETCONTENT, 0, 0);
            for (int i = 0; i < g_ct_n; i++) {
                wchar_t t[300];
                _snwprintf_s(t, 300, _TRUNCATE, L"%s  (by %s)",
                             g_ct_projs[i].title, g_ct_projs[i].author);
                SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t);
            }
            if (r->ok) {
                wchar_t st[128];
                _snwprintf_s(st, 128, _TRUNCATE, L"找到 %d 个项目，双击左侧项目查看版本", g_ct_n);
                SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), st);
            } else {
                SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), r->err[0] ? r->err : L"搜索失败");
            }
        } else if (kind == 1) {
            ct_ver_free(g_ct_vers, g_ct_vn);
            g_ct_vers = (ct_ver*)r->data; g_ct_vn = r->n;
            HWND lb = GetDlgItem(d, IDC_CT_VLIST);
            SendMessageW(lb, LB_RESETCONTENT, 0, 0);
            for (int i = 0; i < g_ct_vn; i++) {
                wchar_t t[300];
                _snwprintf_s(t, 300, _TRUNCATE, L"%s  [%.2f MB]",
                             g_ct_vers[i].name, g_ct_vers[i].size / 1048576.0);
                SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t);
            }
            if (r->ok) {
                wchar_t st[128];
                _snwprintf_s(st, 128, _TRUNCATE, L"找到 %d 个版本，双击右侧版本下载", g_ct_vn);
                SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), st);
            } else {
                SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), r->err[0] ? r->err : L"获取版本失败");
            }
        } else {
            SendMessageW(GetDlgItem(d, IDC_CT_BAR), PBM_SETPOS, r->ok ? 100 : 0, 0);
            if (r->ok) {
                SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), L"下载/安装完成，已放入对应目录或版本");
                ui_log(L"内容下载/安装完成");
                rescan_versions();
            } else {
                SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), r->err[0] ? r->err : L"下载失败");
            }
        }
        free(r);
        mem_trim();
        return 0;
    }
    case WM_CLOSE:
        if (g_ct_busy) {
            SetWindowTextW(GetDlgItem(d, IDC_CT_STATUS), L"正在下载/安装，完成前请不要关闭窗口");
            return 0;
        }
        modal_close();
        return 0;
    }
    return DefWindowProcW(d, msg, w, l);
}

static void open_content(void) {
    if (g_ctwin) { SetForegroundWindow(g_ctwin); return; }
    g_ctwin = modal_create(L"LLclContent", L"下载中心", 560, 450);
    modal_run(g_ctwin);
    g_ctwin = NULL;
    ct_proj_free(g_ct_projs, g_ct_n); g_ct_projs = NULL; g_ct_n = 0;
    ct_ver_free(g_ct_vers, g_ct_vn); g_ct_vers = NULL; g_ct_vn = 0;
    g_ct_busy = 0;
}

/* ================= 整合包安装 ================= */
typedef struct {
    wchar_t* mrpack;
    int ok;
    wchar_t err[640];
} mp_job;
static DWORD WINAPI mp_thread(LPVOID p) {
    mp_job* j = (mp_job*)p;
    dl_progress prog;
    memset(&prog, 0, sizeof(prog));
    j->ok = modpack_install(g_cfg.game_dir, j->mrpack, g_cfg.version_isolate, &prog, j->err, 640);
    free(j->mrpack);
    PostMessageW(g_main, WM_APP_MP_DONE, (WPARAM)j, 0);
    return 0;
}
static void open_modpack(void) {
    if (g_mp_busy) {
        MessageBoxW(g_main, L"整合包正在安装中，请等待完成（进度见主窗口日志）", L"LLcl", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t cur[4096] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_ctwin ? g_ctwin : g_main;
    ofn.lpstrFilter = L"Modrinth 整合包 (*.mrpack)\0*.mrpack\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrFile = cur;
    ofn.nMaxFile = 4096;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"选择 .mrpack 整合包文件";
    if (!GetOpenFileNameW(&ofn)) return;
    mp_job* j = (mp_job*)xalloc(sizeof(mp_job));
    j->mrpack = wdup(cur);
    g_mp_busy = 1;
    ui_log(L"开始安装整合包: %s", cur);
    HANDLE t = CreateThread(NULL, 65536, mp_thread, j, 0, NULL);
    if (t) CloseHandle(t);
}

/* ================= 版本管理（模组 / 光影 / 数据包） ================= */
typedef struct {
    wchar_t* name;
    wchar_t* path;
    uint64_t size;
    int enabled;
} vm_item;

static HWND g_vmwin = NULL;
static wchar_t* g_vm_ver = NULL;
static int g_vm_type = 0;          /* 0=mods 1=shaderpacks 2=datapacks */
static vm_item* g_vm_items = NULL;
static int g_vm_n = 0;

static const wchar_t* vm_subdir(int type) {
    return (type == 1) ? L"shaderpacks" : (type == 2) ? L"datapacks" : L"mods";
}
static const wchar_t* vm_type_name(int type) {
    return (type == 1) ? L"光影" : (type == 2) ? L"数据包" : L"模组";
}
static int vm_ends(const wchar_t* s, const wchar_t* suffix) {
    size_t n = wcslen(s), m = wcslen(suffix);
    return n >= m && _wcsicmp(s + n - m, suffix) == 0;
}
static wchar_t* vm_content_dir(void) {
    wchar_t* root = NULL;
    if (g_cfg.version_isolate && g_vm_ver) {
        wchar_t* a = path_join3(g_cfg.game_dir, L"versions", g_vm_ver);
        root = path_join(a, L"instance");
        free(a);
    } else {
        root = wdup(g_cfg.game_dir ? g_cfg.game_dir : L".");
    }
    wchar_t* dir = path_join(root, vm_subdir(g_vm_type));
    free(root);
    return dir;
}
static void vm_items_free(void) {
    for (int i = 0; i < g_vm_n; i++) {
        free(g_vm_items[i].name);
        free(g_vm_items[i].path);
    }
    free(g_vm_items);
    g_vm_items = NULL;
    g_vm_n = 0;
}
static int __cdecl vm_item_cmp(const void* a, const void* b) {
    return _wcsicmp(((const vm_item*)a)->name, ((const vm_item*)b)->name);
}
static int vm_ok_ext(const wchar_t* name, int type) {
    wchar_t tmp[512];
    wcsncpy_s(tmp, 512, name, _TRUNCATE);
    size_t l = wcslen(tmp);
    if (l > 9 && _wcsicmp(tmp + l - 9, L".disabled") == 0) tmp[l - 9] = 0;
    if (type == 0) return vm_ends(tmp, L".jar");
    return vm_ends(tmp, L".zip") || vm_ends(tmp, L".jar");
}
static void vm_update_add_label(HWND d) {
    SetWindowTextW(GetDlgItem(d, IDC_VM_ADD),
                   g_vm_type == 0 ? L"添加本地模组" : L"添加本地文件");
}
static void vm_status(HWND d) {
    int enabled = 0;
    for (int i = 0; i < g_vm_n; i++) if (g_vm_items[i].enabled) enabled++;
    wchar_t* dir = vm_content_dir();
    wchar_t st[2048];
    _snwprintf_s(st, 2048, _TRUNCATE,
                 L"%d 个%s（启用 %d，禁用 %d）\r\n目录：%s%s",
                 g_vm_n, vm_type_name(g_vm_type), enabled, g_vm_n - enabled, dir,
                 g_vm_type == 2 ? L"\r\n提示：世界专用数据包在 saves\\<世界>\\datapacks，可点“打开文件夹”后进入。" : L"");
    SetWindowTextW(GetDlgItem(d, IDC_VM_STATUS), st);
    free(dir);
}
static void vm_load(HWND d) {
    vm_items_free();
    wchar_t* dir = vm_content_dir();
    if (dir_exists(dir)) {
        const wchar_t* pats[4];
        int np = 0;
        if (g_vm_type == 0) {
            pats[np++] = L"*.jar";
            pats[np++] = L"*.jar.disabled";
        } else {
            pats[np++] = L"*.zip";
            pats[np++] = L"*.zip.disabled";
            if (g_vm_type == 2) {
                pats[np++] = L"*.jar";
                pats[np++] = L"*.jar.disabled";
            }
        }
        for (int k = 0; k < np; k++) {
            wchar_t* pat = path_join(dir, pats[k]);
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(pat, &fd);
            free(pat);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (!vm_ok_ext(fd.cFileName, g_vm_type)) continue;
                vm_item it;
                memset(&it, 0, sizeof(it));
                it.name = wdup(fd.cFileName);
                it.path = path_join(dir, fd.cFileName);
                it.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                it.enabled = !vm_ends(fd.cFileName, L".disabled");
                g_vm_items = (vm_item*)xrealloc(g_vm_items, (size_t)(g_vm_n + 1) * sizeof(vm_item));
                g_vm_items[g_vm_n++] = it;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (g_vm_n > 1) qsort(g_vm_items, (size_t)g_vm_n, sizeof(vm_item), vm_item_cmp);
    HWND lb = GetDlgItem(d, IDC_VM_LIST);
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_vm_n; i++) {
        wchar_t* sz = fmt_bytes(g_vm_items[i].size);
        wchar_t line[600];
        _snwprintf_s(line, 600, _TRUNCATE, L"[%s] %s    (%s)",
                     g_vm_items[i].enabled ? L"启用" : L"禁用",
                     g_vm_items[i].name, sz);
        free(sz);
        int idx = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)line);
        SendMessageW(lb, LB_SETITEMDATA, idx, i);
    }
    vm_update_add_label(d);
    vm_status(d);
    free(dir);
}
static int* vm_get_sel(HWND d, int* out_n) {
    *out_n = 0;
    HWND lb = GetDlgItem(d, IDC_VM_LIST);
    int cnt = (int)SendMessageW(lb, LB_GETSELCOUNT, 0, 0);
    if (cnt <= 0) return NULL;
    if (cnt > g_vm_n) cnt = g_vm_n;
    int* sel = (int*)xalloc((size_t)cnt * sizeof(int));
    int got = (int)SendMessageW(lb, LB_GETSELITEMS, (WPARAM)cnt, (LPARAM)sel);
    if (got <= 0) { free(sel); return NULL; }
    *out_n = got;
    return sel;
}
static void vm_copy_one(HWND d, const wchar_t* src, const wchar_t* dir, int* copied, int* skipped) {
    const wchar_t* name = path_base(src);
    if (!vm_ok_ext(name, g_vm_type)) { (*skipped)++; return; }
    wchar_t* dest = path_join(dir, name);
    if (file_exists(dest)) {
        (*skipped)++;
    } else if (CopyFileW(src, dest, FALSE)) {
        (*copied)++;
    } else {
        (*skipped)++;
    }
    free(dest);
    (void)d;
}
static void vm_add(HWND d) {
    wchar_t* dir = vm_content_dir();
    if (!dir_exists(dir) && !ensure_dir_rec(dir)) {
        MessageBoxW(d, L"无法创建内容目录，请检查游戏目录权限。", L"LLcl", MB_OK | MB_ICONERROR);
        free(dir);
        return;
    }
    wchar_t* buf = (wchar_t*)xalloc(65536 * sizeof(wchar_t));
    buf[0] = 0;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = d;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 65536;
    ofn.lpstrInitialDir = dir;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
    if (g_vm_type == 0)
        ofn.lpstrFilter = L"模组文件 (*.jar)\0*.jar\0所有文件 (*.*)\0*.*\0\0";
    else if (g_vm_type == 1)
        ofn.lpstrFilter = L"光影包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0\0";
    else
        ofn.lpstrFilter = L"数据包 (*.zip;*.jar)\0*.zip;*.jar\0所有文件 (*.*)\0*.*\0\0";
    ofn.lpstrTitle = g_vm_type == 0 ? L"添加本地模组" : L"添加本地文件";
    if (!GetOpenFileNameW(&ofn)) { free(buf); free(dir); return; }
    int copied = 0, skipped = 0;
    wchar_t* p = buf;
    wchar_t* first = p;
    p += wcslen(p) + 1;
    if (*p == 0) {
        vm_copy_one(d, first, dir, &copied, &skipped);
    } else {
        while (*p) {
            wchar_t* src = path_join(first, p);
            vm_copy_one(d, src, dir, &copied, &skipped);
            free(src);
            p += wcslen(p) + 1;
        }
    }
    free(buf);
    free(dir);
    vm_load(d);
    wchar_t msg[256];
    _snwprintf_s(msg, 256, _TRUNCATE, L"已添加 %d 个，跳过 %d 个（同名或格式不符）", copied, skipped);
    ui_log(L"版本 %s 内容管理: %s", g_vm_ver ? g_vm_ver : L"?", msg);
    MessageBoxW(d, msg, L"LLcl", MB_OK | MB_ICONINFORMATION);
}
static void vm_toggle(HWND d) {
    int nsel = 0;
    int* sel = vm_get_sel(d, &nsel);
    if (!sel) { SetWindowTextW(GetDlgItem(d, IDC_VM_STATUS), L"请先选择要启用/禁用的文件"); return; }
    wchar_t* dir = vm_content_dir();
    int changed = 0, failed = 0;
    for (int i = 0; i < nsel; i++) {
        int idx = (int)SendMessageW(GetDlgItem(d, IDC_VM_LIST), LB_GETITEMDATA, sel[i], 0);
        if (idx < 0 || idx >= g_vm_n) continue;
        vm_item* it = &g_vm_items[idx];
        wchar_t* nn;
        if (it->enabled) {
            nn = wcat3(it->name, L".disabled", L"");
        } else {
            size_t l = wcslen(it->name);
            if (l <= 9) continue;
            nn = wdup(it->name);
            nn[l - 9] = 0;
        }
        wchar_t* np = path_join(dir, nn);
        if (file_exists(np)) { failed++; }
        else if (MoveFileW(it->path, np)) { changed++; }
        else { failed++; }
        free(nn); free(np);
    }
    free(sel); free(dir);
    vm_load(d);
    wchar_t msg[256];
    _snwprintf_s(msg, 256, _TRUNCATE, L"已切换 %d 个，失败 %d 个", changed, failed);
    SetWindowTextW(GetDlgItem(d, IDC_VM_STATUS), msg);
}
static void vm_set_all(HWND d, int enable) {
    wchar_t* dir = vm_content_dir();
    int changed = 0, failed = 0;
    for (int i = 0; i < g_vm_n; i++) {
        vm_item* it = &g_vm_items[i];
        if (!!it->enabled == !!enable) continue;
        wchar_t* nn;
        if (enable) {
            size_t l = wcslen(it->name);
            if (l <= 9) continue;
            nn = wdup(it->name);
            nn[l - 9] = 0;
        } else {
            nn = wcat3(it->name, L".disabled", L"");
        }
        wchar_t* np = path_join(dir, nn);
        if (file_exists(np)) failed++;
        else if (MoveFileW(it->path, np)) changed++;
        else failed++;
        free(nn); free(np);
    }
    free(dir);
    vm_load(d);
    wchar_t msg[128];
    _snwprintf_s(msg, 128, _TRUNCATE, L"已%s %d 个，失败 %d 个",
                 enable ? L"全部启用" : L"全部禁用", changed, failed);
    SetWindowTextW(GetDlgItem(d, IDC_VM_STATUS), msg);
}
static void vm_delete(HWND d) {
    int nsel = 0;
    int* sel = vm_get_sel(d, &nsel);
    if (!sel) { SetWindowTextW(GetDlgItem(d, IDC_VM_STATUS), L"请先选择要删除的文件"); return; }
    wchar_t ask[256];
    _snwprintf_s(ask, 256, _TRUNCATE, L"确定删除选中的 %d 个文件？\n此操作不可撤销。", nsel);
    if (MessageBoxW(d, ask, L"LLcl - 版本管理", MB_YESNO | MB_ICONWARNING) != IDYES) { free(sel); return; }
    int ok = 0, failed = 0;
    for (int i = 0; i < nsel; i++) {
        int idx = (int)SendMessageW(GetDlgItem(d, IDC_VM_LIST), LB_GETITEMDATA, sel[i], 0);
        if (idx < 0 || idx >= g_vm_n) continue;
        if (DeleteFileW(g_vm_items[idx].path)) ok++;
        else failed++;
    }
    free(sel);
    vm_load(d);
    wchar_t msg[128];
    _snwprintf_s(msg, 128, _TRUNCATE, L"已删除 %d 个，失败 %d 个", ok, failed);
    SetWindowTextW(GetDlgItem(d, IDC_VM_STATUS), msg);
}
static void vm_open_folder(HWND d) {
    wchar_t* dir = vm_content_dir();
    if (!dir_exists(dir)) ensure_dir_rec(dir);
    if (dir_exists(dir))
        ShellExecuteW(NULL, L"open", dir, NULL, NULL, SW_SHOWNORMAL);
    else
        MessageBoxW(d, L"无法打开目录，请检查游戏目录权限。", L"LLcl", MB_OK | MB_ICONERROR);
    free(dir);
}
static LRESULT CALLBACK vm_proc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_vmwin = d;
        wchar_t t[320];
        _snwprintf_s(t, 320, _TRUNCATE, L"版本：%s", g_vm_ver ? g_vm_ver : L"?");
        mk(0, L"STATIC", t, SS_LEFT, 12, 10, 536, 18, d, IDC_VM_VER);
        mk(0, L"STATIC", L"类型", SS_LEFT, 12, 38, 40, 18, d, -1);
        HWND cb = mk(WS_EX_CLIENTEDGE, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                     54, 34, 160, 200, d, IDC_VM_TYPE);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"模组");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"光影");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"数据包");
        SendMessageW(cb, CB_SETCURSEL, 0, 0);
        mk(0, L"STATIC", L"双击可启用/禁用；禁用文件以 .disabled 结尾", SS_LEFT, 224, 38, 324, 18, d, -1);
        mk(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | WS_TABSTOP |
           LBS_EXTENDEDSEL | LBS_NOTIFY, 12, 62, 536, 250, d, IDC_VM_LIST);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
           12, 320, 536, 40, d, IDC_VM_STATUS);
        mk(0, L"BUTTON", L"添加本地文件", WS_TABSTOP | BS_PUSHBUTTON, 12, 366, 130, 28, d, IDC_VM_ADD);
        mk(0, L"BUTTON", L"启用/禁用", WS_TABSTOP | BS_PUSHBUTTON, 150, 366, 110, 28, d, IDC_VM_TOGGLE);
        mk(0, L"BUTTON", L"删除", WS_TABSTOP | BS_PUSHBUTTON, 268, 366, 90, 28, d, IDC_VM_DELETE);
        mk(0, L"BUTTON", L"刷新", WS_TABSTOP | BS_PUSHBUTTON, 366, 366, 80, 28, d, IDC_VM_REFRESH);
        mk(0, L"BUTTON", L"打开文件夹", WS_TABSTOP | BS_PUSHBUTTON, 454, 366, 94, 28, d, IDC_VM_OPEN);
        mk(0, L"BUTTON", L"全部启用", WS_TABSTOP | BS_PUSHBUTTON, 12, 404, 90, 28, d, 2811);
        mk(0, L"BUTTON", L"全部禁用", WS_TABSTOP | BS_PUSHBUTTON, 110, 404, 90, 28, d, 2812);
        mk(0, L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON, 454, 404, 94, 28, d, IDC_VM_CLOSE);
        vm_load(d);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        int code = HIWORD(w);
        if (id == IDC_VM_TYPE && code == CBN_SELCHANGE) {
            int t = (int)SendMessageW(GetDlgItem(d, IDC_VM_TYPE), CB_GETCURSEL, 0, 0);
            g_vm_type = (t < 0 || t > 2) ? 0 : t;
            vm_load(d);
            return 0;
        }
        if (id == IDC_VM_ADD) { vm_add(d); return 0; }
        if (id == IDC_VM_TOGGLE) { vm_toggle(d); return 0; }
        if (id == IDC_VM_DELETE) { vm_delete(d); return 0; }
        if (id == IDC_VM_REFRESH) { vm_load(d); return 0; }
        if (id == IDC_VM_OPEN) { vm_open_folder(d); return 0; }
        if (id == 2811) { vm_set_all(d, 1); return 0; }
        if (id == 2812) { vm_set_all(d, 0); return 0; }
        if (id == IDC_VM_CLOSE) { modal_close(); return 0; }
        if (id == IDC_VM_LIST && code == LBN_DBLCLK) { vm_toggle(d); return 0; }
        return 0;
    }
    case WM_CLOSE: modal_close(); return 0;
    }
    return DefWindowProcW(d, msg, w, l);
}

static void open_version_mgr(void) {
    if (g_vmwin) { SetForegroundWindow(g_vmwin); return; }
    int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || sel < 0 || sel >= g_nvers) {
        MessageBoxW(g_main, L"请先在版本列表中选择一个版本", L"LLcl", MB_OK | MB_ICONINFORMATION);
        return;
    }
    free(g_vm_ver);
    g_vm_ver = wdup(g_vers[sel].id);
    g_vm_type = 0;
    wchar_t title[320];
    _snwprintf_s(title, 320, _TRUNCATE, L"版本管理 - %s", g_vm_ver);
    g_vmwin = modal_create(L"LLclVersionMgr", title, 560, 448);
    modal_run(g_vmwin);
    g_vmwin = NULL;
    vm_items_free();
    free(g_vm_ver);
    g_vm_ver = NULL;
}

/* ================= 微软登录对话框 ================= */
static HWND g_auth = NULL;
static int g_auth_busy = 0;

typedef struct {
    int mode;                 /* 0=devicecode 1=token */
    wchar_t* token;
    wchar_t* user_code;       /* device flow: 显示给用户 */
    wchar_t* verify_url;
} actx;

static void auth_status(HWND d, const wchar_t* s) {
    if (d) SetWindowTextW(GetDlgItem(d, IDC_A_STATUS), s);
}

static void auth_apply(cfg* c, const wchar_t* uuid, const wchar_t* name,
                       const wchar_t* access, const wchar_t* refresh, const wchar_t* xuid) {
    free(c->msa_uuid); c->msa_uuid = uuid ? wdup(uuid) : NULL;
    free(c->msa_name); c->msa_name = name ? wdup(name) : NULL;
    free(c->msa_access); c->msa_access = (access && access[0]) ? wdup(access) : NULL;
    free(c->msa_refresh); c->msa_refresh = (refresh && refresh[0]) ? wdup(refresh) : NULL;
    free(c->msa_xuid); c->msa_xuid = (xuid && xuid[0]) ? wdup(xuid) : NULL;
    /* 玩家名自动切到正版名 */
    if (name && name[0]) { free(c->player); c->player = wdup(name); }
}

static DWORD WINAPI auth_thread_device(void* p) {
    actx* c = (actx*)p;
    wchar_t err[640] = L"";
    wchar_t code[64] = L"", dcode[4096] = L"", vurl[256] = L"", access[8192] = L"", refresh[8192] = L"";
    int interval = 5, expires_in = 900;
    if (!msa_devicecode(g_cfg.msa_client, code, 64, dcode, 4096, vurl, 256, err, 640,
                        &interval, &expires_in)) {
        PostMessageW(g_auth, WM_APP_A_DONE, 0, (LPARAM)wdup(err));
        free(c); return 0;
    }
    /* 显示设备码并打开浏览器 */
    wchar_t* msg = (wchar_t*)xalloc(1024 * sizeof(wchar_t));
    _snwprintf_s(msg, 1024, _TRUNCATE, L"%s|%s", vurl[0] ? vurl : L"https://microsoft.com/link", code);
    PostMessageW(g_auth, WM_APP_A_CODE, 0, (LPARAM)msg);
    if (!msa_poll_token(g_cfg.msa_client, dcode, interval, access, 8192, refresh, 8192, err, 640)) {
        PostMessageW(g_auth, WM_APP_A_DONE, 0, (LPARAM)wdup(err));
        free(c); return 0;
    }
    /* 设备码返回的 access_token 已含 XboxLive.signin 权限，直接用它走完整链路；
       refresh_token 仅保存下来供以后静默续期，登录时无需再换一次 token。 */
    wchar_t uuid[128] = L"", name[128] = L"", xuid[64] = L"";
    if (!msa_token_login(access, uuid, 128, name, 128, xuid, 64, err, 640)) {
        /* 直接 token 失败时，再用 refresh 换一个 token 重试一次（兼容极端情况） */
        wchar_t err2[640] = L"";
        wchar_t uuid2[128] = L"", name2[128] = L"", xuid2[64] = L"", rn[8192] = L"", acc2[8192] = L"";
        if (!msa_restore(g_cfg.msa_client, refresh, uuid2, 128, name2, 128, xuid2, 64,
                         acc2, 8192, rn, 8192, err2, 640)) {
            PostMessageW(g_auth, WM_APP_A_DONE, 0, (LPARAM)wdup(err));
            free(c); return 0;
        }
        auth_apply(&g_cfg, uuid2, name2, acc2, rn, xuid2);
        cfg_save(g_cfgpath, &g_cfg);
        PostMessageW(g_auth, WM_APP_A_DONE, 1, 0);
        free(c);
        return 0;
    }
    auth_apply(&g_cfg, uuid, name, access, refresh, xuid);
    cfg_save(g_cfgpath, &g_cfg);
    PostMessageW(g_auth, WM_APP_A_DONE, 1, 0);
    free(c);
    return 0;
}

static DWORD WINAPI auth_thread_token(void* p) {
    actx* c = (actx*)p;
    wchar_t err[640] = L"";
    wchar_t uuid[128] = L"", name[128] = L"", xuid[64] = L"";
    if (!msa_token_login(c->token, uuid, 128, name, 128, xuid, 64, err, 640)) {
        PostMessageW(g_auth, WM_APP_A_DONE, 0, (LPARAM)wdup(err));
        free(c->token); free(c); return 0;
    }
    /* 手动 token 没有 refresh_token；保存 access 供本次及后续启动使用。 */
    auth_apply(&g_cfg, uuid, name, c->token, NULL, xuid);
    cfg_save(g_cfgpath, &g_cfg);
    PostMessageW(g_auth, WM_APP_A_DONE, 1, 0);
    free(c->token); free(c);
    return 0;
}

static void auth_start(HWND d, int mode) {
    if (g_auth_busy) return;
    g_auth_busy = 1;
    EnableWindow(GetDlgItem(d, IDC_A_DEVICE), FALSE);
    EnableWindow(GetDlgItem(d, IDC_A_TOKEN), FALSE);
    EnableWindow(GetDlgItem(d, IDC_A_LOGOUT), FALSE);
    EnableWindow(GetDlgItem(d, IDC_A_VERIFY), FALSE);
    actx* c = (actx*)xalloc(sizeof(actx));
    memset(c, 0, sizeof(*c));
    c->mode = mode;
    if (mode == 1) {
        wchar_t t[4096];
        GetWindowTextW(GetDlgItem(d, IDC_A_EDIT), t, 4096);
        c->token = wdup(t[0] ? t : L"");
        auth_status(d, L"正在验证 Token…");
    } else {
        auth_status(d, L"正在申请设备码…");
    }
    HANDLE th = CreateThread(NULL, 512 * 1024, mode == 1 ? auth_thread_token : auth_thread_device, c, 0, NULL);
    if (th) CloseHandle(th);
}

static LRESULT CALLBACK auth_proc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_auth = d;   /* 后台线程要 PostMessage 到本窗口 */
        mk(0, L"STATIC", L"账号与登录：离线玩家名在“设置”中修改；正版使用设备码 OAuth", SS_LEFT, 14, 10, 412, 18, d, -1);
        /* 唯一的状态框：多行只读，足够完整显示设备码网址、错误原因，不再被单行裁切 */
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
           14, 30, 412, 70, d, IDC_A_STATUS);
        mk(0, L"BUTTON", L"设备码登录", WS_TABSTOP | BS_PUSHBUTTON, 14, 108, 130, 30, d, IDC_A_DEVICE);
        mk(0, L"BUTTON", L"手填 Token", WS_TABSTOP | BS_PUSHBUTTON, 154, 108, 130, 30, d, IDC_A_TOKEN);
        mk(0, L"BUTTON", L"退出登录", WS_TABSTOP | BS_PUSHBUTTON, 294, 108, 130, 30, d, IDC_A_LOGOUT);
        HWND e1 = mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
                     14, 146, 270, 24, d, IDC_A_EDIT);
        HWND b1 = mk(0, L"BUTTON", L"验证", WS_TABSTOP | BS_PUSHBUTTON,
                     294, 146, 130, 24, d, IDC_A_VERIFY);
        ShowWindow(e1, SW_HIDE);   /* 高级功能：点“手填 Token”才展开 */
        ShowWindow(b1, SW_HIDE);
        mk(0, L"STATIC",
           L"设备码登录：点“设备码登录”后会自动打开浏览器，在网页输入界面显示的代码并授权即可。\r\n"
           L"手填 Token：高级用户可粘贴带 XboxLive.signin 权限的 access_token 后点“验证”。\r\n"
           L"登录成功后自动保存，下次启动自动续期；离线登录始终可用。",
           SS_LEFT, 14, 180, 412, 70, d, -1);
        /* 账号状态 */
        wchar_t st[256];
        if (g_cfg.msa_uuid && g_cfg.msa_uuid[0] &&
            ((g_cfg.msa_access && g_cfg.msa_access[0]) || (g_cfg.msa_refresh && g_cfg.msa_refresh[0])))
            _snwprintf_s(st, 256, _TRUNCATE, L"已登录正版账号: %s (UUID %.*s…)\r\n启动时自动续期并使用正版档案",
                         g_cfg.msa_name ? g_cfg.msa_name : L"?", 8, g_cfg.msa_uuid);
        else
            _snwprintf_s(st, 256, _TRUNCATE, L"当前为离线登录（玩家名: %s）\r\n点“设备码登录”可绑定微软正版账号；离线名可在“设置”中修改",
                         g_cfg.player ? g_cfg.player : L"Steve");
        auth_status(d, st);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_A_DEVICE) { auth_start(d, 0); return 0; }
        if (id == IDC_A_TOKEN) {
            ShowWindow(GetDlgItem(d, IDC_A_EDIT), SW_SHOW);
            ShowWindow(GetDlgItem(d, IDC_A_VERIFY), SW_SHOW);
            auth_status(d, L"粘贴微软 access_token 后点“验证”（token 需含 XboxLive.signin 权限）");
            return 0;
        }
        if (id == IDC_A_VERIFY) { auth_start(d, 1); return 0; }
        if (id == IDC_A_LOGOUT) {
            free(g_cfg.msa_refresh); g_cfg.msa_refresh = NULL;
            free(g_cfg.msa_access); g_cfg.msa_access = NULL;
            free(g_cfg.msa_uuid); g_cfg.msa_uuid = NULL;
            free(g_cfg.msa_name); g_cfg.msa_name = NULL;
            free(g_cfg.msa_xuid); g_cfg.msa_xuid = NULL;
            cfg_save(g_cfgpath, &g_cfg);
            auth_status(d, L"已退出正版登录，当前为离线模式");
            ui_log(L"微软登录已退出（回到离线模式）");
            refresh_account_label();
            return 0;
        }
        return 0;
    }
    case WM_APP_A_CODE: {
        wchar_t* msg = (wchar_t*)l;
        if (msg) {
            wchar_t* sep = wcschr(msg, L'|');
            wchar_t* vurl = msg, * code = NULL;
            if (sep) { *sep = 0; code = sep + 1; }
            wchar_t t[512];
            if (code)
                _snwprintf_s(t, 512, _TRUNCATE,
                             L"请在浏览器打开下面网址并输入代码：\r\n网址: %s\r\n代码: %s\r\n授权完成后会自动继续，无需手动关闭本窗口。",
                             vurl, code);
            else
                _snwprintf_s(t, 512, _TRUNCATE, L"%s", vurl);
            auth_status(d, t);
            if (vurl[0]) ShellExecuteW(NULL, L"open", vurl, NULL, NULL, SW_SHOWNORMAL);
            free(msg);
        }
        return 0;
    }
    case WM_APP_A_DONE: {
        g_auth_busy = 0;
        EnableWindow(GetDlgItem(d, IDC_A_DEVICE), TRUE);
        EnableWindow(GetDlgItem(d, IDC_A_TOKEN), TRUE);
        EnableWindow(GetDlgItem(d, IDC_A_LOGOUT), TRUE);
        EnableWindow(GetDlgItem(d, IDC_A_VERIFY), TRUE);
        if (w) {
            wchar_t st[256];
            _snwprintf_s(st, 256, _TRUNCATE, L"登录成功！\r\n账号: %s (UUID %.*s…)\r\n可关闭本窗口并启动游戏",
                         g_cfg.msa_name ? g_cfg.msa_name : L"?", 8, g_cfg.msa_uuid ? g_cfg.msa_uuid : L"");
            auth_status(d, st);
            ui_log(L"微软正版登录成功: %s (%s)", g_cfg.msa_name ? g_cfg.msa_name : L"?", g_cfg.msa_uuid ? g_cfg.msa_uuid : L"");
            refresh_account_label();
        } else {
            wchar_t* e = (wchar_t*)l;
            auth_status(d, e && e[0] ? e : L"登录失败");
            if (e) free(e);
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_auth_busy) {
            auth_status(d, L"正在登录/轮询授权，完成前请不要关闭窗口");
            return 0;
        }
        modal_close();
        return 0;
    }
    return DefWindowProcW(d, msg, w, l);
}

/* ================= 远程联机 + 服务器列表 ================= */
/* 网络引擎在工作线程里跑；这里只做三件事：
 *   1. 把 UI 操作翻译成 net_* 调用；
 *   2. 把 net_* 事件 PostMessage 回主线程更新界面；
 *   3. 维护 servers.dat（服务器列表）与 LAN 扫描。 */

typedef struct { wchar_t* msg; int ev; } net_msg;
static wchar_t g_pending_connect[512] = L"";
static wchar_t g_last_net_invite[1024] = L"";

static void net_update_main_label(void) {
    if (!g_net_label) return;
    net_status st;
    net_get_status(&st);
    wchar_t t[512];
    if (st.active) {
        if (st.is_host) {
            _snwprintf_s(t, 512, _TRUNCATE, L"远程联机：主机房间 %s（%s，%d 人）",
                         st.room[0] ? st.room : L"……",
                         st.mode == NET_MODE_RELAY ? L"中转" : L"直连", st.peers);
        } else {
            _snwprintf_s(t, 512, _TRUNCATE, L"远程联机：已加入房间，本机 %s",
                         st.address[0] ? st.address : L"……");
        }
    } else {
        wcsncpy_s(t, 512, L"远程联机：未开启（直连 / 中转，支持 LAN 列表）", _TRUNCATE);
    }
    SetWindowTextW(g_net_label, t);
}

static void net_ui_event(int ev, const wchar_t* msg, void* user) {
    (void)user;
    if (!g_main) return;
    net_msg* m = (net_msg*)xalloc(sizeof(net_msg));
    m->ev = ev;
    m->msg = wdup(msg ? msg : L"");
    PostMessageW(g_main, WM_APP_NET_EV, (WPARAM)ev, (LPARAM)m);
}

/* ---------------- servers.dat ---------------- */
typedef struct {
    srv_entry* list;
    int n;
    int kind;       /* 2 = LAN 扫描结果 */
    wchar_t err[256];
} srv_jobres;

static void mp_free_servers(void) {
    if (g_mp_servers) {
        server_list_free(g_mp_servers, g_mp_n);
        g_mp_servers = NULL;
    }
    g_mp_n = 0;
}

static void mp_load_servers(void) {
    mp_free_servers();
    if (!g_cfg.game_dir) return;
    wchar_t* path = path_join(g_cfg.game_dir, L"servers.dat");
    if (file_exists(path)) servers_load_path(path, &g_mp_servers, &g_mp_n);
    free(path);
}

static void mp_save_servers(void) {
    if (!g_cfg.game_dir) return;
    wchar_t* path = path_join(g_cfg.game_dir, L"servers.dat");
    if (!servers_save_path(path, g_mp_servers, g_mp_n))
        ui_log(L"保存 servers.dat 失败: %s", path);
    free(path);
}

static void mp_fill_list(HWND d) {
    HWND lb = GetDlgItem(d, IDC_MP_LIST);
    if (!lb) return;
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_mp_n; i++) {
        wchar_t t[512];
        _snwprintf_s(t, 512, _TRUNCATE, L"%s%s  —  %s",
                     g_mp_servers[i].hidden ? L"[隐藏] " : L"",
                     g_mp_servers[i].name ? g_mp_servers[i].name : L"(未命名)",
                     g_mp_servers[i].ip ? g_mp_servers[i].ip : L"");
        int idx = (int)SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)t);
        SendMessageW(lb, LB_SETITEMDATA, idx, i);
    }
}

static int mp_find_server_ip(const wchar_t* ip) {
    for (int i = 0; i < g_mp_n; i++)
        if (g_mp_servers[i].ip && weqi(g_mp_servers[i].ip, ip)) return i;
    return -1;
}

static void mp_append_server(const srv_entry* src) {
    if (!src || !src->ip) return;
    if (mp_find_server_ip(src->ip) >= 0) return;
    g_mp_servers = (srv_entry*)xrealloc(g_mp_servers, (size_t)(g_mp_n + 1) * sizeof(srv_entry));
    srv_entry* dst = &g_mp_servers[g_mp_n++];
    memset(dst, 0, sizeof(*dst));
    dst->name = src->name ? wdup(src->name) : NULL;
    dst->ip = wdup(src->ip);
    dst->hidden = src->hidden;
    dst->icon = src->icon ? wdup(src->icon) : NULL;
}

static void mp_add_server(HWND d) {
    wchar_t text[512] = L"";
    GetWindowTextW(GetDlgItem(d, IDC_MP_ADDEDIT), text, 512);
    wchar_t* p = text;
    while (*p == L' ') p++;
    wchar_t* sp = wcsrchr(p, L' ');
    if (!sp || sp == p) {
        SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), L"格式：名称 IP，例如：我的服务器 127.0.0.1:25565");
        return;
    }
    *sp = 0;
    wchar_t* ip = sp + 1;
    while (*ip == L' ') ip++;
    if (!ip[0]) {
        SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), L"IP 不能为空");
        return;
    }
    wchar_t full[512];
    if (wcschr(ip, L':')) wcsncpy_s(full, 512, ip, _TRUNCATE);
    else _snwprintf_s(full, 512, _TRUNCATE, L"%s:25565", ip);
    srv_entry e;
    memset(&e, 0, sizeof(e));
    e.name = wdup(p);
    e.ip = wdup(full);
    mp_append_server(&e);
    free(e.name);
    free(e.ip);
    mp_save_servers();
    mp_fill_list(d);
    SetWindowTextW(GetDlgItem(d, IDC_MP_ADDEDIT), L"");
    SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), L"服务器已添加并保存");
}

static void mp_delete_selected(HWND d) {
    HWND lb = GetDlgItem(d, IDC_MP_LIST);
    int cnt = (int)SendMessageW(lb, LB_GETSELCOUNT, 0, 0);
    if (cnt <= 0) {
        SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), L"请先选中要删除的服务器");
        return;
    }
    int* sel = (int*)xalloc((size_t)cnt * sizeof(int));
    SendMessageW(lb, LB_GETSELITEMS, cnt, (LPARAM)sel);
    for (int i = cnt - 1; i >= 0; i--) {
        int idx = (int)SendMessageW(lb, LB_GETITEMDATA, sel[i], 0);
        if (idx < 0 || idx >= g_mp_n) continue;
        free(g_mp_servers[idx].name);
        free(g_mp_servers[idx].ip);
        free(g_mp_servers[idx].icon);
        for (int j = idx; j < g_mp_n - 1; j++) g_mp_servers[j] = g_mp_servers[j + 1];
        g_mp_n--;
    }
    free(sel);
    mp_save_servers();
    mp_fill_list(d);
    SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), L"已删除并保存");
}

static void mp_connect_selected(HWND d) {
    HWND lb = GetDlgItem(d, IDC_MP_LIST);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) {
        SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), L"请先选择一个服务器");
        return;
    }
    int idx = (int)SendMessageW(lb, LB_GETITEMDATA, sel, 0);
    if (idx < 0 || idx >= g_mp_n || !g_mp_servers[idx].ip) return;
    wcsncpy_s(g_pending_connect, 512, g_mp_servers[idx].ip, _TRUNCATE);
    modal_close();
}

static DWORD WINAPI mp_lan_thread(LPVOID p) {
    HWND target = (HWND)p;
    srv_jobres* r = (srv_jobres*)xalloc(sizeof(srv_jobres));
    memset(r, 0, sizeof(*r));
    r->kind = 2;

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        wcsncpy_s(r->err, 256, L"创建 UDP 套接字失败", _TRUNCATE);
        PostMessageW(target, WM_APP_SRV_DONE, 0, (LPARAM)r);
        return 0;
    }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(4445);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
        wcsncpy_s(r->err, 256, L"无法监听 UDP 4445（游戏刚刚启动时可能短暂占用，请稍后重试）", _TRUNCATE);
        closesocket(s);
        PostMessageW(target, WM_APP_SRV_DONE, 0, (LPARAM)r);
        return 0;
    }
    ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.2.60");
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));
    DWORD tv = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    for (int i = 0; i < 9; i++) {
        char buf[1024];
        sockaddr_in from;
        int fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        if (strncmp(buf, "[MOTD]", 6) != 0) continue;
        char* motd = buf + 6;
        char* me = strstr(motd, "[/MOTD]");
        if (!me) continue;
        *me = 0;
        char* ad = strstr(me + 7, "[AD]");
        if (!ad) continue;
        ad += 4;
        char* ae = strstr(ad, "[/AD]");
        if (!ae) continue;
        *ae = 0;
        int port = atoi(ad);
        if (port <= 0 || port > 65535) continue;
        char ip[64] = "";
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        wchar_t* wip = u2w(ip);
        wchar_t* wmotd = u2w(motd);
        if (!wip || !wmotd) { free(wip); free(wmotd); continue; }
        wchar_t full[128];
        _snwprintf_s(full, 128, _TRUNCATE, L"%s:%d", wip, port);
        srv_entry* e = (srv_entry*)xalloc(sizeof(srv_entry));
        memset(e, 0, sizeof(*e));
        e->name = wmotd[0] ? wdup(wmotd) : wdup(L"局域网游戏");
        e->ip = wdup(full);
        int dup = 0;
        for (int k = 0; k < r->n; k++) if (weqi(r->list[k].ip, e->ip)) { dup = 1; break; }
        if (dup) {
            free(e->name); free(e->ip); free(e);
        } else {
            r->list = (srv_entry*)xrealloc(r->list, (size_t)(r->n + 1) * sizeof(srv_entry));
            r->list[r->n++] = *e;
            free(e);
        }
        free(wip);
        free(wmotd);
    }
    closesocket(s);
    PostMessageW(target, WM_APP_SRV_DONE, 0, (LPARAM)r);
    return 0;
}

static void mp_lan_scan(HWND d) {
    if (g_mp_busy) return;
    g_mp_busy = 1;
    SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), L"正在扫描局域网游戏（UDP 4445，约 5 秒）…");
    EnableWindow(GetDlgItem(d, IDC_MP_LANSCAN), FALSE);
    HANDLE t = CreateThread(NULL, 128 * 1024, mp_lan_thread, d, 0, NULL);
    if (t) CloseHandle(t);
    else { g_mp_busy = 0; EnableWindow(GetDlgItem(d, IDC_MP_LANSCAN), TRUE); }
}

/* ---------------- 远程联机操作 ---------------- */
static void mp_sync_net(HWND d) {
    if (!d) return;
    net_status st;
    net_get_status(&st);
    net_update_main_label();

    wchar_t status[1200];
    if (st.status[0]) wcsncpy_s(status, 1200, st.status, _TRUNCATE);
    else wcsncpy_s(status, 1200, L"未联机。创建房间后把邀请码发给朋友；加入房间后打开游戏多人列表即可。", _TRUNCATE);
    SetWindowTextW(GetDlgItem(d, IDC_NET_STATUS), status);

    wchar_t info[1600];
    if (st.active && st.is_host) {
        if (st.mode == NET_MODE_RELAY)
            _snwprintf_s(info, 1600, _TRUNCATE,
                L"房间码：%s\r\n中转：%s\r\n邀请码已生成，点“复制邀请码”发给朋友。",
                st.room[0] ? st.room : L"……", st.relay[0] ? st.relay : L"-");
        else
            _snwprintf_s(info, 1600, _TRUNCATE,
                L"房间码：%s\r\n本机地址：%s（MC 端口 %d）\r\n直连需要主机允许防火墙；外网访问需路由器映射该 TCP 端口，或用中转模式。",
                st.room[0] ? st.room : L"……", st.address[0] ? st.address : L"-", st.mc_port);
    } else if (st.active) {
        _snwprintf_s(info, 1600, _TRUNCATE,
            L"本机游戏地址：%s\r\n打开 Minecraft → 多人游戏，局域网列表会出现房间；也可以“直接连接”这个地址。\r\n请保持本窗口所在启动器运行。",
            st.address[0] ? st.address : L"……");
    } else {
        wcsncpy_s(info, 1600,
            L"主机：先在游戏里“对局域网开放”，再点“创建房间”（端口留空会自动读日志）。\r\n"
            L"玩家：粘贴邀请码后点“加入房间”，然后打开游戏多人列表。\r\n"
            L"直连适合有公网/UPnP 的主机；中转适合双方都在 NAT 后面（需一个可访问的中转服务器）。",
            _TRUNCATE);
    }
    SetWindowTextW(GetDlgItem(d, IDC_NET_INFO), info);

    if (st.active && st.is_host && st.invite[0]) {
        if (wcscmp(g_last_net_invite, st.invite) != 0) {
            SetWindowTextW(GetDlgItem(d, IDC_NET_INVITE), st.invite);
            wcsncpy_s(g_last_net_invite, 1024, st.invite, _TRUNCATE);
        }
    }

    EnableWindow(GetDlgItem(d, IDC_NET_CREATE), !st.active);
    EnableWindow(GetDlgItem(d, IDC_NET_JOIN), !st.active);
    EnableWindow(GetDlgItem(d, IDC_NET_STOP), st.active);
    EnableWindow(GetDlgItem(d, IDC_NET_COPY), st.active && st.is_host);
    EnableWindow(GetDlgItem(d, IDC_NET_COPYADDR), st.active && !st.is_host);
    EnableWindow(GetDlgItem(d, IDC_NET_RELAY), !st.active);
    EnableWindow(GetDlgItem(d, IDC_NET_PORT), !st.active);
    EnableWindow(GetDlgItem(d, IDC_NET_MODE_D), !st.active);
    EnableWindow(GetDlgItem(d, IDC_NET_MODE_R), !st.active);
    EnableWindow(GetDlgItem(d, IDC_NET_INVITE), !st.active);
}

static void mp_create_room(HWND d) {
    int mode = IsDlgButtonChecked(d, IDC_NET_MODE_R) == BST_CHECKED ? NET_MODE_RELAY : NET_MODE_DIRECT;
    wchar_t relay[256] = L"";
    GetWindowTextW(GetDlgItem(d, IDC_NET_RELAY), relay, 256);
    wchar_t portbuf[32] = L"";
    GetWindowTextW(GetDlgItem(d, IDC_NET_PORT), portbuf, 32);
    int mcport = _wtoi(portbuf);
    if (mcport < 0 || mcport > 65535) {
        MessageBoxW(d, L"MC 端口应为 1–65535，留空表示自动从游戏日志检测。", L"LLcl", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (mode == NET_MODE_RELAY && !relay[0]) {
        MessageBoxW(d, L"中转模式需要填写中转服务器地址，例如 1.2.3.4:25566。\n"
                       L"可以在有公网 IP 的机器上运行：LLcl.exe --relay 25566", L"LLcl", MB_OK | MB_ICONINFORMATION);
        return;
    }

    wchar_t* logs = NULL;
    if (g_cfg.game_dir) {
        if (g_cfg.version_isolate && g_cur_ok && g_cur.id) {
            wchar_t* vdir = path_join3(g_cfg.game_dir, L"versions", g_cur.id);
            wchar_t* inst = path_join(vdir, L"instance");
            logs = path_join(inst, L"logs");
            free(inst);
            free(vdir);
        } else {
            logs = path_join(g_cfg.game_dir, L"logs");
        }
    }
    wchar_t err[512] = L"";
    int ok = net_create_host(mode, relay, mcport, g_cfg.net_port, logs, err, 512);
    free(logs);
    if (!ok) {
        MessageBoxW(d, err[0] ? err : L"创建房间失败", L"LLcl - 远程联机", MB_OK | MB_ICONERROR);
        return;
    }
    free(g_cfg.relay_server); g_cfg.relay_server = relay[0] ? wdup(relay) : NULL;
    g_cfg.net_mode = mode;
    cfg_save(g_cfgpath, &g_cfg);
    ui_log(L"远程联机：创建%s房间…", mode == NET_MODE_RELAY ? L"中转" : L"直连");
    g_last_net_invite[0] = 0;
    mp_sync_net(d);
}

static void mp_join_room(HWND d) {
    wchar_t code[1024] = L"";
    GetWindowTextW(GetDlgItem(d, IDC_NET_INVITE), code, 1024);
    wchar_t* p = code;
    while (*p == L' ' || *p == L'\r' || *p == L'\n') p++;
    size_t l = wcslen(p);
    while (l && (p[l - 1] == L' ' || p[l - 1] == L'\r' || p[l - 1] == L'\n')) p[--l] = 0;
    if (!p[0]) {
        MessageBoxW(d, L"请先粘贴主机发来的邀请码（LLCL1|…）", L"LLcl", MB_OK | MB_ICONINFORMATION);
        return;
    }
    wchar_t err[512] = L"";
    if (!net_join(p, err, 512)) {
        MessageBoxW(d, err[0] ? err : L"加入房间失败", L"LLcl - 远程联机", MB_OK | MB_ICONERROR);
        return;
    }
    free(g_cfg.net_invite); g_cfg.net_invite = wdup(p);
    cfg_save(g_cfgpath, &g_cfg);
    ui_log(L"远程联机：正在加入房间…");
    mp_sync_net(d);
}

static void mp_copy_invite(HWND d) {
    wchar_t inv[1024] = L"";
    if (!net_copy_invite(inv, 1024)) {
        MessageBoxW(d, L"邀请码还没生成，请稍候。", L"LLcl", MB_OK | MB_ICONINFORMATION);
        return;
    }
    net_clipboard_copy(d, inv);
    SetWindowTextW(GetDlgItem(d, IDC_NET_STATUS), L"邀请码已复制到剪贴板，发给朋友即可。");
}

static void mp_copy_addr(HWND d) {
    net_status st;
    net_get_status(&st);
    wchar_t addr[256] = L"";
    if (st.active && !st.is_host && st.mc_port > 0)
        _snwprintf_s(addr, 256, _TRUNCATE, L"127.0.0.1:%d", st.mc_port);
    if (!addr[0]) {
        MessageBoxW(d, L"当前没有可复制的游戏地址。", L"LLcl", MB_OK | MB_ICONINFORMATION);
        return;
    }
    net_clipboard_copy(d, addr);
    SetWindowTextW(GetDlgItem(d, IDC_NET_STATUS), L"游戏地址已复制，可在“直接连接”里粘贴。");
}

/* ---------------- 多人游戏窗口 ---------------- */
static void open_multiplayer(void) {
    if (g_mpwin) { SetForegroundWindow(g_mpwin); return; }
    if (!g_cfg.game_dir || !dir_exists(g_cfg.game_dir)) {
        MessageBoxW(g_main, L"游戏目录不存在，请先在“设置”中指定", L"LLcl", MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_pending_connect[0] = 0;
    g_last_net_invite[0] = 0;
    g_mpwin = modal_create(L"LLclMultiplayer", L"远程联机 / 多人游戏", 680, 600);
    modal_run(g_mpwin);
    g_mpwin = NULL;
    mp_free_servers();
    g_mp_busy = 0;
    net_update_main_label();
    if (g_pending_connect[0]) {
        launch_set_server(g_pending_connect);
        g_pending_connect[0] = 0;
        do_launch();
    }
}

static LRESULT CALLBACK mp_proc(HWND d, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        g_mpwin = d;
        mk(0, L"BUTTON", L"远程联机房间（加入后游戏 LAN 列表会直接出现）",
           BS_GROUPBOX, 12, 8, 656, 228, d, -1);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
           24, 30, 632, 36, d, IDC_NET_STATUS);
        mk(0, L"BUTTON", L"直连（自动 UPnP，外网需端口映射）", BS_AUTORADIOBUTTON | WS_GROUP,
           24, 74, 270, 20, d, IDC_NET_MODE_D);
        mk(0, L"BUTTON", L"中转（需可访问的中转服务器，免公网 IP）", BS_AUTORADIOBUTTON,
           300, 74, 360, 20, d, IDC_NET_MODE_R);
        mk(0, L"STATIC", L"中转服务器", SS_LEFT, 24, 102, 80, 20, d, -1);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
           108, 100, 300, 24, d, IDC_NET_RELAY);
        mk(0, L"STATIC", L"MC 端口", SS_LEFT, 424, 102, 60, 20, d, -1);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
           488, 100, 60, 24, d, IDC_NET_PORT);
        mk(0, L"STATIC", L"留空自动检测", SS_LEFT, 556, 102, 100, 20, d, -1);
        mk(0, L"BUTTON", L"创建房间", WS_TABSTOP | BS_PUSHBUTTON, 24, 134, 130, 32, d, IDC_NET_CREATE);
        mk(0, L"BUTTON", L"停止联机", WS_TABSTOP | BS_PUSHBUTTON, 162, 134, 110, 32, d, IDC_NET_STOP);
        mk(0, L"BUTTON", L"复制邀请码", WS_TABSTOP | BS_PUSHBUTTON, 280, 134, 130, 32, d, IDC_NET_COPY);
        mk(0, L"BUTTON", L"复制游戏地址", WS_TABSTOP | BS_PUSHBUTTON, 418, 134, 140, 32, d, IDC_NET_COPYADDR);
        mk(0, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
           24, 174, 632, 52, d, IDC_NET_INFO);

        mk(0, L"BUTTON", L"加入房间", BS_GROUPBOX, 12, 244, 656, 90, d, -1);
        mk(0, L"STATIC", L"邀请码", SS_LEFT, 24, 268, 60, 20, d, -1);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
           88, 264, 470, 26, d, IDC_NET_INVITE);
        mk(0, L"BUTTON", L"加入房间", WS_TABSTOP | BS_PUSHBUTTON, 566, 262, 90, 30, d, IDC_NET_JOIN);

        mk(0, L"BUTTON", L"服务器列表（servers.dat） / 局域网扫描",
           BS_GROUPBOX, 12, 342, 656, 226, d, -1);
        g_mp_list = mk(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                       WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_EXTENDEDSEL,
                       24, 364, 632, 120, d, IDC_MP_LIST);
        mk(0, L"BUTTON", L"连接", WS_TABSTOP | BS_PUSHBUTTON, 24, 494, 84, 28, d, IDC_MP_CONNECT);
        mk(0, L"BUTTON", L"添加", WS_TABSTOP | BS_PUSHBUTTON, 116, 494, 84, 28, d, IDC_MP_ADD);
        mk(0, L"BUTTON", L"删除", WS_TABSTOP | BS_PUSHBUTTON, 208, 494, 84, 28, d, IDC_MP_DELETE);
        mk(0, L"BUTTON", L"刷新", WS_TABSTOP | BS_PUSHBUTTON, 300, 494, 84, 28, d, IDC_MP_REFRESH);
        mk(0, L"BUTTON", L"扫描局域网", WS_TABSTOP | BS_PUSHBUTTON, 392, 494, 110, 28, d, IDC_MP_LANSCAN);
        mk(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL,
           24, 528, 380, 24, d, IDC_MP_ADDEDIT);
        mk(0, L"STATIC", L"格式：名称 IP（空格分隔）", SS_LEFT,
           412, 530, 240, 18, d, -1);
        mk(0, L"STATIC", L"", SS_LEFT, 24, 558, 400, 18, d, IDC_MP_STATUS);
        mk(0, L"BUTTON", L"关闭", WS_TABSTOP | BS_PUSHBUTTON, 566, 552, 90, 30, d, IDC_MP_CLOSE);

        CheckRadioButton(d, IDC_NET_MODE_D, IDC_NET_MODE_R,
                         g_cfg.net_mode == NET_MODE_RELAY ? IDC_NET_MODE_R : IDC_NET_MODE_D);
        if (g_cfg.relay_server) SetWindowTextW(GetDlgItem(d, IDC_NET_RELAY), g_cfg.relay_server);
        EnableWindow(GetDlgItem(d, IDC_NET_RELAY), g_cfg.net_mode != NET_MODE_RELAY);
        mp_load_servers();
        mp_fill_list(d);
        mp_sync_net(d);
        SetTimer(d, 1, 1000, NULL);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_NET_CREATE) { mp_create_room(d); return 0; }
        if (id == IDC_NET_JOIN) { mp_join_room(d); return 0; }
        if (id == IDC_NET_STOP) { net_stop(); mp_sync_net(d); return 0; }
        if (id == IDC_NET_COPY) { mp_copy_invite(d); return 0; }
        if (id == IDC_NET_COPYADDR) { mp_copy_addr(d); return 0; }
        if (id == IDC_NET_MODE_D || id == IDC_NET_MODE_R) {
            EnableWindow(GetDlgItem(d, IDC_NET_RELAY), id == IDC_NET_MODE_R);
            return 0;
        }
        if (id == IDC_MP_CONNECT) { mp_connect_selected(d); return 0; }
        if (id == IDC_MP_ADD) { mp_add_server(d); return 0; }
        if (id == IDC_MP_DELETE) { mp_delete_selected(d); return 0; }
        if (id == IDC_MP_REFRESH) {
            mp_load_servers();
            mp_fill_list(d);
            SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), L"服务器列表已刷新");
            return 0;
        }
        if (id == IDC_MP_LANSCAN) { mp_lan_scan(d); return 0; }
        if (id == IDC_MP_CLOSE) { modal_close(); return 0; }
        if (id == IDC_MP_LIST && HIWORD(w) == LBN_DBLCLK) { mp_connect_selected(d); return 0; }
        return 0;
    }
    case WM_APP_NET_EV: {
        net_msg* m = (net_msg*)l;
        if (m) {
            if (m->ev == NET_EV_LOG) ui_log(L"%s", m->msg);
            else if (m->ev == NET_EV_STATUS) ui_log(L"联机: %s", m->msg);
            else if (m->ev == NET_EV_STOPPED) ui_log(L"联机已停止");
            free(m->msg);
            free(m);
        }
        mp_sync_net(d);
        return 0;
    }
    case WM_APP_SRV_DONE: {
        srv_jobres* r = (srv_jobres*)l;
        g_mp_busy = 0;
        EnableWindow(GetDlgItem(d, IDC_MP_LANSCAN), TRUE);
        if (r) {
            if (r->kind == 2) {
                if (r->err[0]) {
                    SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), r->err);
                } else {
                    for (int i = 0; i < r->n; i++) mp_append_server(&r->list[i]);
                    if (r->n) {
                        mp_save_servers();
                        mp_fill_list(d);
                        wchar_t t[128];
                        _snwprintf_s(t, 128, _TRUNCATE, L"发现并添加了 %d 个局域网游戏", r->n);
                        SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), t);
                    } else {
                        SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS),
                                       L"未发现局域网游戏。请确认对方已“对局域网开放”，且在同一网络；远程房间请用邀请码。");
                    }
                }
                for (int i = 0; i < r->n; i++) {
                    free(r->list[i].name);
                    free(r->list[i].ip);
                    free(r->list[i].icon);
                }
                free(r->list);
            }
            free(r);
        }
        return 0;
    }
    case WM_TIMER:
        if (w == 1) { mp_sync_net(d); return 0; }
        break;
    case WM_CLOSE:
        if (g_mp_busy) {
            SetWindowTextW(GetDlgItem(d, IDC_MP_STATUS), L"正在扫描，请稍候…");
            return 0;
        }
        modal_close();
        return 0;
    case WM_DESTROY:
        KillTimer(d, 1);
        return 0;
    }
    return DefWindowProcW(d, msg, w, l);
}

/* ================= 微软登录对话框 ================= */
static void open_auth(void) {
    if (g_auth) { SetForegroundWindow(g_auth); return; }
    g_auth = modal_create(L"LLclAuth", L"账号与登录", 450, 300);
    modal_run(g_auth);
    g_auth = NULL;
}

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE: {
        /* 左列：版本列表 + 账号摘要 + 日志；玩家名统一放到设置/账号对话框 */
        mk(0, L"STATIC", L"游戏版本", SS_LEFT, 12, 10, 200, 18, h, -1);
        g_list = mk(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                    WS_BORDER | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY,
                    12, 30, 260, 300, h, IDC_LIST);
        g_acc_label = mk(0, L"STATIC", L"", SS_LEFT, 12, 336, 628, 18, h, -1);
        /* 右列：操作按钮 + 状态（下载/加载器/整合包/内容合并入口） */
        mk(0, L"STATIC", L"操作", SS_LEFT, 288, 10, 200, 18, h, -1);
        g_btn_launch = mk(0, L"BUTTON", L"启动游戏", WS_TABSTOP | BS_PUSHBUTTON,
                          288, 30, 340, 40, h, IDC_BTN_LAUNCH);
        mk(0, L"BUTTON", L"设置", WS_TABSTOP | BS_PUSHBUTTON, 288, 74, 340, 30, h, IDC_BTN_SETTINGS);
        mk(0, L"BUTTON", L"下载与安装（游戏 / 模组加载器）", WS_TABSTOP | BS_PUSHBUTTON, 288, 106, 340, 30, h, IDC_BTN_DOWNLOAD);
        mk(0, L"BUTTON", L"内容中心（Mod / 光影 / 资源包 / 整合包）", WS_TABSTOP | BS_PUSHBUTTON, 288, 138, 340, 30, h, IDC_BTN_CONTENT);
        mk(0, L"BUTTON", L"账号与登录（离线 / 微软正版）", WS_TABSTOP | BS_PUSHBUTTON, 288, 170, 340, 30, h, IDC_BTN_AUTH);
        mk(0, L"BUTTON", L"磁盘清理", WS_TABSTOP | BS_PUSHBUTTON, 288, 202, 340, 30, h, IDC_BTN_CLEAN);
        mk(0, L"BUTTON", L"版本管理（模组 / 光影 / 数据包）", WS_TABSTOP | BS_PUSHBUTTON, 288, 234, 340, 30, h, IDC_BTN_VERMGR);
        mk(0, L"BUTTON", L"远程联机（房间 / 服务器）", WS_TABSTOP | BS_PUSHBUTTON, 288, 266, 340, 34, h, IDC_BTN_MULTI);
        g_net_label = mk(0, L"STATIC", L"远程联机：未开启（直连 / 中转，支持 LAN 列表）",
                         SS_LEFT, 288, 304, 340, 36, h, IDC_NET_LABEL);
        g_status = mk(WS_EX_CLIENTEDGE, L"EDIT", L"",
                      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                      288, 356, 340, 56, h, IDC_HINT);
        /* 日志区（整宽） */
        mk(0, L"STATIC", L"日志（调试）", SS_LEFT, 12, 420, 300, 18, h, -1);
        g_log = mk(WS_EX_CLIENTEDGE, L"EDIT", L"",
                   ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                   12, 440, 628, 170, h, IDC_LOG);
        populate_versions();
        int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
        select_version(sel);
        refresh_account_label();
        ui_log(L"LLcl %s — Minecraft Java 启动器", LLCL_VERSION);
        ui_log(L"游戏目录: %s", g_cfg.game_dir ? g_cfg.game_dir : L"(未设置)");
        ui_log(L"扫描到 %d 个版本", g_nvers);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_BTN_LAUNCH) { do_launch(); return 0; }
        if (id == IDC_BTN_SETTINGS) { open_settings(); return 0; }
        if (id == IDC_BTN_CLEAN) { open_cleanup(); return 0; }
        if (id == IDC_BTN_DOWNLOAD) { open_download(); return 0; }
        if (id == IDC_BTN_CONTENT) { open_content(); return 0; }
        if (id == IDC_BTN_AUTH) { open_auth(); return 0; }
        if (id == IDC_BTN_VERMGR) { open_version_mgr(); return 0; }
        if (id == IDC_BTN_MULTI) { open_multiplayer(); return 0; }
        if (id == IDC_LIST && HIWORD(w) == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
            select_version(sel);
            return 0;
        }
        return 0;
    }
    case WM_APP_JAVA_DONE: {
        jres* res = (jres*)w;
        javas_free(g_javas, g_njavas);
        g_javas = res->list;
        g_njavas = res->n;
        free(res);
        /* 重新探测后旧的下标可能已经指向另一个 Java，必须重新按版本选择。 */
        g_java_sel = -1;
        if (g_njavas > 0) {
            pick_java_for_version();
            if (g_java_sel < 0) g_java_sel = 0;
        }
        ui_log(L"Java 探测完成: %d 个可用", g_njavas);
        for (int i = 0; i < g_njavas && i < 8; i++)
            ui_log(L"  %s (%d位) %s", g_javas[i].fullver, g_javas[i].is64 ? 64 : 32, g_javas[i].path);
        if (g_njavas == 0) ui_log(L"  未检测到 Java —— 老机器常见，请安装 Java 8/17/21 后重新检测");
        if (g_settings) {
            HWND cb = GetDlgItem(g_settings, IDC_S_COMBO);
            if (cb) settings_fill_combo(g_settings);
        }
        java_hint_text();
        mem_trim();
        return 0;
    }
    case WM_APP_LAUNCH_DONE: {
        lres* res = (lres*)w;
        lresult* r = &res->r;
        g_launching = 0;
        EnableWindow(g_btn_launch, TRUE);
        if (r->ok) {
            ui_log(L"启动成功! PID=%lu  总耗时 %llu ms（原生库 %llu ms，命令行 %llu ms，进程创建 %llu ms）",
                   (unsigned long)r->pid, (unsigned long long)(r->us_total / 1000),
                   (unsigned long long)(r->us_natives / 1000),
                   (unsigned long long)(r->us_build / 1000),
                   (unsigned long long)(r->us_spawn / 1000));
            if (r->missing_libs > 0)
                ui_log(L"警告: %d 个库文件缺失（游戏可能启动失败）；请用“下载与安装”重新补齐该版本", r->missing_libs);
            if (r->early_exit) {
                ui_log(L"警告: Java 进程启动后立即退出（退出码 %lu），游戏未能启动，原因见下方输出",
                       (unsigned long)r->exit_code);
                show_log_tail(r->log_path);
            }
            java_hint_text();
        } else {
            ui_log(L"启动失败: %s", r->err);
            MessageBoxW(g_main, r->err, L"LLcl - 启动失败", MB_OK | MB_ICONERROR);
            java_hint_text();
        }
        free(res);
        return 0;
    }
    case WM_APP_JD_DONE: {
        jd_result* r = (jd_result*)l;
        if (r->ok) {
            ui_log(L"Java 自动下载完成，继续启动…");
            free(r);
            mem_trim();
            g_launching = 0;
            EnableWindow(g_btn_launch, TRUE);
            do_launch();   /* 这次 find_java_for 能找到 runtime，直接启动 */
        } else {
            ui_log(L"Java 自动下载失败: %s", r->err);
            MessageBoxW(g_main, r->err[0] ? r->err : L"Java 下载失败",
                        L"LLcl", MB_OK | MB_ICONERROR);
            g_launching = 0;
            EnableWindow(g_btn_launch, TRUE);
            free(r);
        }
        return 0;
    }
    case WM_APP_MP_DONE: {
        mp_job* j = (mp_job*)w;
        g_mp_busy = 0;
        if (j->ok) {
            ui_log(L"整合包安装完成！");
            MessageBoxW(g_main, L"整合包安装完成！在版本列表选择新加载器版本即可启动。",
                        L"LLcl", MB_OK | MB_ICONINFORMATION);
            rescan_versions();
        } else {
            ui_log(L"整合包安装失败: %s", j->err);
            MessageBoxW(g_main, j->err, L"LLcl - 整合包安装失败", MB_OK | MB_ICONERROR);
        }
        free(j);
        mem_trim();
        return 0;
    }
    case WM_APP_SCAN_PROG: {
        if (g_cleanup) {
            wchar_t t[128];
            _snwprintf_s(t, 128, _TRUNCATE, L"扫描中… %d 个文件", (int)w);
            SetWindowTextW(GetDlgItem(g_cleanup, IDC_C_STATUS), t);
        }
        return 0;
    }
    case WM_APP_SCAN_DONE: {
        sres* res = (sres*)w;
        if (g_cats) ccat_free(g_cats, g_ncats);
        g_cats = res->cats;
        g_ncats = res->n;
        free(res);
        g_scanning = 0;
        if (g_cleanup) {
            HWND lb = GetDlgItem(g_cleanup, IDC_C_LIST);
            SendMessageW(lb, LB_RESETCONTENT, 0, 0);
            int fc = 0;
            for (int i = 0; i < g_ncats; i++) {
                wchar_t* sz = fmt_bytes(g_cats[i].bytes);
                wsb b;
                wsb_init(&b);
                wsb_addf(&b, L"%s — %d 个文件, %s", g_cats[i].label, g_cats[i].files, sz);
                SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)b.b);
                wsb_free(&b);
                free(sz);
                fc += g_cats[i].files;
            }
            wchar_t t[128];
            if (g_ncats == 0) _snwprintf_s(t, 128, _TRUNCATE, L"没有发现可清理的无用文件");
            else _snwprintf_s(t, 128, _TRUNCATE, L"扫描完成: %d 类，共 %d 个文件", g_ncats, fc);
            SetWindowTextW(GetDlgItem(g_cleanup, IDC_C_STATUS), t);
            EnableWindow(GetDlgItem(g_cleanup, IDC_C_SCAN), TRUE);
            EnableWindow(GetDlgItem(g_cleanup, IDC_C_CLEAN), TRUE);
        }
        return 0;
    }
    case WM_APP_CLEAN_DONE: {
        cres* res = (cres*)w;
        g_cleaning = 0;
        wchar_t* f = fmt_bytes(res->freed);
        ui_log(L"磁盘清理: 释放 %s", f);
        if (g_cleanup) {
            wchar_t t[128];
            _snwprintf_s(t, 128, _TRUNCATE, L"已释放 %s。重新扫描查看剩余项目。", f);
            SetWindowTextW(GetDlgItem(g_cleanup, IDC_C_STATUS), t);
            EnableWindow(GetDlgItem(g_cleanup, IDC_C_SCAN), TRUE);
            EnableWindow(GetDlgItem(g_cleanup, IDC_C_CLEAN), TRUE);
        }
        free(f);
        free(res);
        return 0;
    }
    case WM_APP_NET_EV: {
        net_msg* m = (net_msg*)l;
        if (m) {
            if (m->ev == NET_EV_LOG) ui_log(L"%s", m->msg);
            else if (m->ev == NET_EV_STATUS) ui_log(L"联机: %s", m->msg);
            else if (m->ev == NET_EV_STOPPED) ui_log(L"联机已停止");
            free(m->msg);
            free(m);
        }
        net_update_main_label();
        if (g_mpwin) mp_sync_net(g_mpwin);
        return 0;
    }
    case WM_CTLCOLORSTATIC: return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_CTLCOLOREDIT: return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        {
            net_set_event_cb(NULL, NULL);
            net_stop();
            int sel = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_nvers) {
                free(g_cfg.last_version);
                g_cfg.last_version = wdup(g_vers[sel].id);
            }
        }
        cfg_save(g_cfgpath, &g_cfg);
        PostQuitMessage(0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

/* ---------------- init / run ---------------- */
void ui_init(HINSTANCE h) {
    g_hinst = h;
    /* SHBrowseForFolder(BIF_NEWDIALOGSTYLE)/ShellExecute 需要 COM 初始化 */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();
    /* 读取真实 DPI，用于控件坐标等比缩放（Win7 可用的基础 API） */
    HDC hdc = GetDC(NULL);
    if (hdc) {
        /* 基准是 96 DPI；旧实现直接把 120/144 当百分比，125%/150% 缩放下
           控件会偏小，文字被截断。 */
        g_dpix = MulDiv(GetDeviceCaps(hdc, LOGPIXELSX), 100, 96);
        g_dpiy = MulDiv(GetDeviceCaps(hdc, LOGPIXELSY), 100, 96);
        ReleaseDC(NULL, hdc);
        if (g_dpix < 100) g_dpix = 100;
        if (g_dpiy < 100) g_dpiy = 100;
    }
    /* 注册进度条等通用控件类（comctl32 已静态链接） */
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    else
        g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    cfg_init(&g_cfg);
    net_set_event_cb(net_ui_event, NULL);
    wchar_t* exedir = get_exe_dir();
    g_cfgpath = path_join(exedir, L"LLcl.ini");
    cfg_load(g_cfgpath, &g_cfg);
    /* 下载线程数规范化：仅允许 8/16，默认 8；同步给下载引擎 */
    g_cfg.dl_threads = (g_cfg.dl_threads == 16) ? 16 : 8;
    g_dl_threads = g_cfg.dl_threads;
    if (!g_cfg.player || !g_cfg.player[0]) {
        free(g_cfg.player);
        g_cfg.player = wdup(L"Steve");
    }
    if (!g_cfg.game_dir || !g_cfg.game_dir[0]) {
        /* 启动器自己的 mc 文件夹：exe 同级 .minecraft\，发布版默认目录。
           注意 ini 里 game_dir= 会读出空串而不是 NULL。 */
        free(g_cfg.game_dir);
        wchar_t* own = path_join(exedir, L".minecraft");
        ensure_dir_rec(own);
        g_cfg.game_dir = own;
    }
    free(exedir);

    /* scan versions */
    if (g_cfg.game_dir && dir_exists(g_cfg.game_dir))
        scan_versions(g_cfg.game_dir, &g_vers, &g_nvers);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.hInstance = h;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = L"LLclMain";
    RegisterClassW(&wc);
    wc.lpfnWndProc = settings_proc;
    wc.lpszClassName = L"LLclSettings";
    RegisterClassW(&wc);
    wc.lpfnWndProc = cleanup_proc;
    wc.lpszClassName = L"LLclCleanup";
    RegisterClassW(&wc);
    wc.lpfnWndProc = dl_proc;
    wc.lpszClassName = L"LLclDownload";
    RegisterClassW(&wc);
    wc.lpfnWndProc = ld_proc;
    wc.lpszClassName = L"LLclLoader";
    RegisterClassW(&wc);
    wc.lpfnWndProc = content_proc;
    wc.lpszClassName = L"LLclContent";
    RegisterClassW(&wc);
    wc.lpfnWndProc = auth_proc;
    wc.lpszClassName = L"LLclAuth";
    RegisterClassW(&wc);
    wc.lpfnWndProc = mp_proc;
    wc.lpszClassName = L"LLclMultiplayer";
    RegisterClassW(&wc);
    wc.lpfnWndProc = vm_proc;
    wc.lpszClassName = L"LLclVersionMgr";
    RegisterClassW(&wc);
}

int ui_run(void) {
    g_main = CreateWindowExW(0, L"LLclMain", L"LLcl - Minecraft Java 启动器",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             SX(660), SY(660),
                             NULL, NULL, g_hinst, NULL);
    if (!g_main) return 1;
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);
    /* 注意：WM_CREATE 时 g_main 尚未赋值（CreateWindowExW 返回前已发送），
       探测线程要 PostMessage 到 g_main，必须在 g_main 就绪后再启动 */
    do_detect();
    mem_trim();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageW(g_main, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
