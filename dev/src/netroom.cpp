/* LLcl - netroom.cpp : 远程联机（直连 / 中转 / Minecraft LAN 公告）
 *
 * 为什么不做虚拟网卡：
 *   安装 TAP/WinPcap 驱动需要管理员权限、签名驱动和额外内存，和“单文件、<500KB、
 *   Win7 可用”的目标冲突。这里用应用层方案达到同样的使用效果：
 *     - 主机把 Minecraft “对局域网开放”的 TCP 端口接入房间；
 *     - 加入方在本机得到一个 127.0.0.1:25565 代理；
 *     - 启动器同时模拟 Minecraft 的 LAN 组播公告，游戏多人列表会直接出现房间。
 *
 * 两种模式：
 *   直连：主机监听一个 TCP 端口，尽量用 UPnP 自动映射；邀请码里带公网/局域网地址。
 *   中转：双方都主动连接中转服务器（LLcl.exe --relay 25566），不需要公网 IP。
 */
#define _CRT_RAND_S
#include "llcl.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")

#define NR_MAX_SOCKS      24
#define NR_LINE           512
#define NR_BUF            8192
#define NR_RELAY_ROOMS    16
#define NR_RELAY_PEERS    8
#define NR_INVITE_CAP     1024

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    CRITICAL_SECTION cs;
    int      gen;
    volatile LONG stop;
    int      active;
    int      is_host;
    int      mode;
    int      peers;
    int      mc_port;
    int      req_port;
    wchar_t  room[32];
    wchar_t  invite[NR_INVITE_CAP];
    wchar_t  address[256];
    wchar_t  relay[256];
    wchar_t  status[512];
    wchar_t  logs_dir[512];
    SOCKET   listener;
    SOCKET   control;
    SOCKET   socks[NR_MAX_SOCKS];
    int      nsocks;
} nr_state;

static nr_state g_nr;
static CRITICAL_SECTION g_cs;
static LONG g_cs_ready = 0;
static LONG g_wsa_ready = 0;
static net_event_cb g_ev_cb = NULL;
static void* g_ev_user = NULL;

static void cli_printf(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);
    cli_print(buf);
}

static void nr_cs_init(void) {
    if (InterlockedCompareExchange(&g_cs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_cs);
        InterlockedExchange(&g_cs_ready, 2);
    } else {
        while (g_cs_ready == 1) Sleep(0);
    }
}

static int nr_wsa_init(void) {
    LONG v = InterlockedCompareExchange(&g_wsa_ready, 1, 0);
    if (v == 0) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) {
            InterlockedExchange(&g_wsa_ready, 0);
            return 0;
        }
        InterlockedExchange(&g_wsa_ready, 2);
    } else {
        while (g_wsa_ready == 1) Sleep(0);
    }
    return g_wsa_ready == 2;
}

void net_set_event_cb(net_event_cb cb, void* user) {
    nr_cs_init();
    EnterCriticalSection(&g_cs);
    g_ev_cb = cb;
    g_ev_user = user;
    LeaveCriticalSection(&g_cs);
}

static void nr_emit_raw(int ev, const wchar_t* msg) {
    net_event_cb cb;
    void* user;
    EnterCriticalSection(&g_cs);
    cb = g_ev_cb;
    user = g_ev_user;
    LeaveCriticalSection(&g_cs);
    if (cb) cb(ev, msg, user);
}

static void nr_emit(int gen, int ev, const wchar_t* fmt, ...) {
    wchar_t buf[1200];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1200, _TRUNCATE, fmt, ap);
    va_end(ap);
    net_event_cb cb = NULL;
    void* user = NULL;
    EnterCriticalSection(&g_cs);
    if (gen != g_nr.gen) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    if (ev == NET_EV_STATUS || ev == NET_EV_STOPPED) {
        wcsncpy_s(g_nr.status, 512, buf, _TRUNCATE);
    }
    if (ev == NET_EV_STOPPED) g_nr.active = 0;
    cb = g_ev_cb;
    user = g_ev_user;
    LeaveCriticalSection(&g_cs);
    if (cb) cb(ev, buf, user);
}

static void nr_status(int gen, const wchar_t* fmt, ...) {
    wchar_t buf[1200];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1200, _TRUNCATE, fmt, ap);
    va_end(ap);
    nr_emit(gen, NET_EV_STATUS, L"%s", buf);
}

static void nr_log(int gen, const wchar_t* fmt, ...) {
    wchar_t buf[1200];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1200, _TRUNCATE, fmt, ap);
    va_end(ap);
    nr_emit(gen, NET_EV_LOG, L"%s", buf);
}

static int nr_stopped(int gen) {
    int r;
    EnterCriticalSection(&g_cs);
    r = (gen != g_nr.gen) || (g_nr.stop != 0);
    LeaveCriticalSection(&g_cs);
    return r;
}

static void nr_fail(int gen, const wchar_t* fmt, ...) {
    wchar_t buf[1200];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 1200, _TRUNCATE, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_cs);
    int live = (gen == g_nr.gen);
    if (live) {
        g_nr.active = 0;
        g_nr.stop = 1;
        wcsncpy_s(g_nr.status, 512, buf, _TRUNCATE);
    }
    LeaveCriticalSection(&g_cs);
    if (!live) return;
    nr_emit_raw(NET_EV_STATUS, buf);
    nr_emit_raw(NET_EV_STOPPED, buf);
}

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */
static unsigned nr_rand(void) {
    unsigned v = 0;
    rand_s(&v);
    return v;
}

static void nr_token(wchar_t* out, int n) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) out[i] = (wchar_t)hex[nr_rand() & 15];
    out[n] = 0;
}

static void nr_roomcode(wchar_t* out, int n) {
    static const char al[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    for (int i = 0; i < n; i++) out[i] = (wchar_t)al[nr_rand() % (sizeof(al) - 1)];
    out[n] = 0;
}

static int nr_b64_enc(const unsigned char* in, int n, char* out, int cap) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        if (o + 4 >= cap) return 0;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? T[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int nr_b64_dec(const char* in, unsigned char* out, int cap) {
    int rev[256];
    for (int i = 0; i < 256; i++) rev[i] = -1;
    const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 0; i < 64; i++) rev[(unsigned char)T[i]] = i;
    int o = 0, bits = 0;
    unsigned acc = 0;
    for (const char* p = in; *p; p++) {
        if (*p == '=') break;
        int v = rev[(unsigned char)*p];
        if (v < 0) return 0;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return 0;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    return o;
}

static void nr_name_b64(const wchar_t* name, char* out, int cap) {
    char* u8 = w2u(name ? name : L"LLcl");
    if (!u8) { strncpy_s(out, cap, "TExjbA", _TRUNCATE); return; }
    nr_b64_enc((const unsigned char*)u8, (int)strlen(u8), out, cap);
    free(u8);
}

static void nr_name_from_b64(const char* b64, wchar_t* out, int cap) {
    unsigned char buf[256];
    int n = nr_b64_dec(b64, buf, (int)sizeof(buf) - 1);
    if (n <= 0) { wcsncpy_s(out, cap, L"LLcl 房间", _TRUNCATE); return; }
    buf[n] = 0;
    wchar_t* w = u2w((const char*)buf);
    if (!w) { wcsncpy_s(out, cap, L"LLcl 房间", _TRUNCATE); return; }
    wcsncpy_s(out, cap, w, _TRUNCATE);
    free(w);
}

static int nr_split(const wchar_t* s, wchar_t fields[][256], int max) {
    int n = 0;
    const wchar_t* p = s;
    while (n < max) {
        const wchar_t* q = wcschr(p, L'|');
        size_t len = q ? (size_t)(q - p) : wcslen(p);
        if (len >= 256) return 0;
        wcsncpy_s(fields[n], 256, p, len);
        fields[n][len] = 0;
        n++;
        if (!q) break;
        p = q + 1;
    }
    return n;
}

static void nr_lan_ip(wchar_t* out, int cap) {
    wcsncpy_s(out, cap, L"127.0.0.1", _TRUNCATE);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;
    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(80);
    to.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(s, (sockaddr*)&to, sizeof(to)) == 0) {
        sockaddr_in me;
        int len = sizeof(me);
        if (getsockname(s, (sockaddr*)&me, &len) == 0) {
            char ip[64] = "";
            if (inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip))) {
                wchar_t* w = u2w(ip);
                if (w && w[0] && !wstartswith(w, L"127.")) wcsncpy_s(out, cap, w, _TRUNCATE);
                free(w);
            }
        }
    }
    closesocket(s);
}

static int nr_parse_hostport(const wchar_t* s, wchar_t* host, int hcap, int* port) {
    if (!s) return 0;
    while (*s == L' ') s++;
    if (wstartswith(s, L"tcp://")) s += 6;
    if (wstartswith(s, L"http://") || wstartswith(s, L"https://")) {
        const wchar_t* p = wcschr(s, L'/');
        p = p ? wcschr(p + 1, L'/') : NULL;
        s = p ? p + 1 : s;
    }
    const wchar_t* colon = wcsrchr(s, L':');
    if (colon && colon != s) {
        int p = _wtoi(colon + 1);
        if (p <= 0 || p > 65535) return 0;
        size_t hl = (size_t)(colon - s);
        if (hl >= (size_t)hcap) return 0;
        wcsncpy_s(host, hcap, s, hl);
        host[hl] = 0;
        *port = p;
    } else {
        wcsncpy_s(host, hcap, s, _TRUNCATE);
        *port = 25566;
    }
    return host[0] != 0;
}

static void nr_sock_opts(SOCKET s) {
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    DWORD tv = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
}

static int nr_send_all(SOCKET s, const char* data, int len) {
    int off = 0;
    while (off < len) {
        int n = send(s, data + off, len - off, 0);
        if (n > 0) { off += n; continue; }
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            timeval tv;
            tv.tv_sec = 10;
            tv.tv_usec = 0;
            if (select(0, NULL, &wf, NULL, &tv) > 0) continue;
        }
        return 0;
    }
    return 1;
}

static int nr_send_line(SOCKET s, const char* line) {
    char buf[NR_LINE];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s\n", line);
    if (n <= 0) return 0;
    return nr_send_all(s, buf, n);
}

/* 返回字节数；0 = 对端关闭；-1 = 错误；-2 = 超时 */
static int nr_recv_line(SOCKET s, char* out, int cap, DWORD timeout_ms) {
    DWORD tv = timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    int n = 0;
    for (;;) {
        char c;
        int r = recv(s, &c, 1, 0);
        if (r == 1) {
            if (c == '\n') break;
            if (c != '\r' && n < cap - 1) out[n++] = c;
        } else if (r == 0) {
            if (n == 0) return 0;
            break;
        } else {
            int e = WSAGetLastError();
            if (e == WSAETIMEDOUT) return -2;
            return -1;
        }
    }
    out[n] = 0;
    return n;
}

static int nr_make_listener(int req, SOCKET* out, int* actual) {
    int start = req > 0 ? req : 25566;
    int end = req > 0 ? req : 25620;
    for (int p = start; p <= end; p++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return 0;
        BOOL excl = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
        sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)p);
        a.sin_addr.s_addr = INADDR_ANY;
        if (bind(s, (sockaddr*)&a, sizeof(a)) == 0 && listen(s, SOMAXCONN) == 0) {
            int len = sizeof(a);
            if (getsockname(s, (sockaddr*)&a, &len) == 0) *actual = ntohs(a.sin_port);
            else *actual = p;
            *out = s;
            return 1;
        }
        closesocket(s);
        if (req > 0) break;
    }
    return 0;
}

static SOCKET nr_connect_addr(const wchar_t* host, int port, int timeout_ms) {
    if (!host || !host[0]) return INVALID_SOCKET;
    char* h = w2u(host);
    if (!h) return INVALID_SOCKET;
    char ps[16];
    _snprintf_s(ps, sizeof(ps), _TRUNCATE, "%d", port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = NULL;
    SOCKET s = INVALID_SOCKET;
    if (getaddrinfo(h, ps, &hints, &res) == 0 && res) {
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s != INVALID_SOCKET) {
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);
            int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);
            if (rc == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                fd_set wf, ef;
                FD_ZERO(&wf); FD_ZERO(&ef);
                FD_SET(s, &wf); FD_SET(s, &ef);
                timeval tv;
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                int sr = select(0, NULL, &wf, &ef, &tv);
                if (sr > 0 && FD_ISSET(s, &wf)) {
                    int so = 0, sl = sizeof(so);
                    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so, &sl) != 0 || so != 0)
                        rc = SOCKET_ERROR;
                    else rc = 0;
                } else rc = SOCKET_ERROR;
            } else if (rc == 0) {
                rc = 0;
            }
            nb = 0;
            ioctlsocket(s, FIONBIO, &nb);
            if (rc != 0) {
                closesocket(s);
                s = INVALID_SOCKET;
            } else {
                nr_sock_opts(s);
            }
        }
        freeaddrinfo(res);
    }
    free(h);
    return s;
}

static void nr_track_socket(int gen, SOCKET s) {
    if (s == INVALID_SOCKET) return;
    EnterCriticalSection(&g_cs);
    if (gen == g_nr.gen && g_nr.nsocks < NR_MAX_SOCKS) g_nr.socks[g_nr.nsocks++] = s;
    LeaveCriticalSection(&g_cs);
}

static void nr_untrack_socket(int gen, SOCKET s) {
    EnterCriticalSection(&g_cs);
    if (gen == g_nr.gen) {
        for (int i = 0; i < g_nr.nsocks; i++) {
            if (g_nr.socks[i] == s) {
                g_nr.socks[i] = g_nr.socks[--g_nr.nsocks];
                break;
            }
        }
    }
    LeaveCriticalSection(&g_cs);
}

static void nr_peer_add(int gen) {
    int n = 0;
    EnterCriticalSection(&g_cs);
    if (gen == g_nr.gen) {
        if (g_nr.peers < 999) g_nr.peers++;
        n = g_nr.peers;
    }
    LeaveCriticalSection(&g_cs);
    if (n) nr_status(gen, L"有玩家连接到房间（当前 %d 人）", n);
}

static void nr_peer_del(int gen) {
    int n = -1;
    EnterCriticalSection(&g_cs);
    if (gen == g_nr.gen && g_nr.peers > 0) {
        g_nr.peers--;
        n = g_nr.peers;
    }
    LeaveCriticalSection(&g_cs);
    if (n >= 0) nr_log(gen, L"玩家离开房间（当前 %d 人）", n);
}

static void nr_pump(SOCKET a, SOCKET b, int gen) {
    char buf[NR_BUF];
    int ao = 1, bo = 1;
    while (!nr_stopped(gen) && (ao || bo)) {
        fd_set rf;
        FD_ZERO(&rf);
        if (ao) FD_SET(a, &rf);
        if (bo) FD_SET(b, &rf);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int sr = select(0, &rf, NULL, NULL, &tv);
        if (sr <= 0) continue;
        if (ao && FD_ISSET(a, &rf)) {
            int n = recv(a, buf, sizeof(buf), 0);
            if (n > 0) {
                if (!nr_send_all(b, buf, n)) break;
            } else if (n == 0) {
                ao = 0;
                shutdown(b, SD_SEND);
            } else {
                int e = WSAGetLastError();
                if (e != WSAETIMEDOUT) break;
            }
        }
        if (bo && FD_ISSET(b, &rf)) {
            int n = recv(b, buf, sizeof(buf), 0);
            if (n > 0) {
                if (!nr_send_all(a, buf, n)) break;
            } else if (n == 0) {
                bo = 0;
                shutdown(a, SD_SEND);
            } else {
                int e = WSAGetLastError();
                if (e != WSAETIMEDOUT) break;
            }
        }
    }
}

static void nr_spawn(LPTHREAD_START_ROUTINE fn, void* arg, DWORD stack) {
    HANDLE h = CreateThread(NULL, stack, fn, arg, 0, NULL);
    if (h) CloseHandle(h);
    else free(arg);
}

/* ------------------------------------------------------------------ */
/* 邀请码                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    int mode;
    wchar_t name[64];
    wchar_t pub[64];
    wchar_t lan[64];
    wchar_t relay_host[128];
    int relay_port;
    int port;
    wchar_t code[32];
    wchar_t token[32];
} nr_inv;

static void nr_invite_direct(const wchar_t* name, const wchar_t* pub, const wchar_t* lan,
                             int port, const wchar_t* token, wchar_t* out, int cap) {
    char nb[512];
    nr_name_b64(name, nb, sizeof(nb));
    wchar_t* t = u2w(nb);
    _snwprintf_s(out, cap, _TRUNCATE, L"LLCL1|D|%s|%s|%s|%d|%s",
                 t ? t : L"TExjbA", pub && pub[0] ? pub : L"-", lan && lan[0] ? lan : L"-",
                 port, token);
    free(t);
}

static void nr_invite_relay(const wchar_t* name, const wchar_t* host, int rport,
                            const wchar_t* code, const wchar_t* token,
                            wchar_t* out, int cap) {
    char nb[512];
    nr_name_b64(name, nb, sizeof(nb));
    wchar_t* t = u2w(nb);
    _snwprintf_s(out, cap, _TRUNCATE, L"LLCL1|R|%s|%s|%d|%s|%s",
                 t ? t : L"TExjbA", host, rport, code, token);
    free(t);
}

static int nr_parse_invite(const wchar_t* invite, nr_inv* inv) {
    wchar_t f[9][256];
    memset(inv, 0, sizeof(*inv));
    int n = nr_split(invite, f, 9);
    if (n < 7 || !weq(f[0], L"LLCL1")) return 0;
    {
        char* nb = w2u(f[2]);
        if (nb) { nr_name_from_b64(nb, inv->name, 64); free(nb); }
        else wcsncpy_s(inv->name, 64, L"LLcl 房间", _TRUNCATE);
    }
    if (weq(f[1], L"D")) {
        inv->mode = NET_MODE_DIRECT;
        wcsncpy_s(inv->pub, 64, f[3], _TRUNCATE);
        wcsncpy_s(inv->lan, 64, f[4], _TRUNCATE);
        inv->port = _wtoi(f[5]);
        wcsncpy_s(inv->token, 32, f[6], _TRUNCATE);
        if (inv->port <= 0 || inv->port > 65535) return 0;
        if (!inv->token[0]) return 0;
        return 1;
    }
    if (weq(f[1], L"R")) {
        int rp = _wtoi(f[4]);
        if (!nr_parse_hostport(f[3], inv->relay_host, 128, &rp)) return 0;
        inv->mode = NET_MODE_RELAY;
        inv->relay_port = rp;
        wcsncpy_s(inv->code, 32, f[5], _TRUNCATE);
        wcsncpy_s(inv->token, 32, f[6], _TRUNCATE);
        return inv->code[0] && inv->token[0];
    }
    return 0;
}

int net_copy_invite(wchar_t* out, size_t cap) {
    int ok = 0;
    EnterCriticalSection(&g_cs);
    if (g_nr.invite[0] && cap) {
        wcsncpy_s(out, cap, g_nr.invite, _TRUNCATE);
        ok = 1;
    }
    LeaveCriticalSection(&g_cs);
    return ok;
}

void net_clipboard_copy(HWND owner, const wchar_t* text) {
    if (!text) return;
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t n = (wcslen(text) + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, text, n);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

/* ------------------------------------------------------------------ */
/* Minecraft 局域网端口自动检测（读 latest.log）                        */
/* ------------------------------------------------------------------ */
static int nr_log_port_once(const wchar_t* logs_dir) {
    if (!logs_dir || !logs_dir[0]) return 0;
    wchar_t* path = path_join(logs_dir, L"latest.log");
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) { CloseHandle(h); return 0; }
    DWORD want = (DWORD)(sz.QuadPart > (512 * 1024) ? (512 * 1024) : sz.QuadPart);
    LARGE_INTEGER off;
    off.QuadPart = sz.QuadPart - want;
    SetFilePointerEx(h, off, NULL, FILE_BEGIN);
    char* buf = (char*)xalloc(want + 1);
    DWORD got = 0;
    ReadFile(h, buf, want, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;

    static const char* pats[] = {
        "Started serving on ",
        "Started serving on port ",
        "Local game hosted on port ",
        "Serving on port ",
        "Opened to LAN on port "
    };
    int best_pos = -1, best_port = 0;
    for (int i = 0; i < (int)(sizeof(pats) / sizeof(pats[0])); i++) {
        const char* p = buf;
        int last = -1;
        while ((p = strstr(p, pats[i])) != NULL) { last = (int)(p - buf); p += 1; }
        if (last < 0) continue;
        const char* q = buf + last + strlen(pats[i]);
        while (*q == ' ') q++;
        int v = atoi(q);
        if (v >= 1024 && v <= 65535 && last > best_pos) {
            best_pos = last;
            best_port = v;
        }
    }
    free(buf);
    return best_port;
}

static int nr_wait_log_port(int gen, const wchar_t* logs_dir, int timeout_ms) {
    int waited = 0, announced = 0;
    while (!nr_stopped(gen) && waited < timeout_ms) {
        int p = nr_log_port_once(logs_dir);
        if (p) return p;
        if (!announced) {
            nr_status(gen, L"等待 Minecraft 开启局域网…请进入世界后点“对局域网开放”");
            announced = 1;
        }
        Sleep(700);
        waited += 700;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 直连主机                                                            */
/* ------------------------------------------------------------------ */
typedef struct { int gen; SOCKET s; wchar_t token[256]; int mc_port; } nr_conn_arg;

static DWORD WINAPI nr_direct_host_conn(LPVOID p) {
    nr_conn_arg* a = (nr_conn_arg*)p;
    char line[NR_LINE];
    int r = nr_recv_line(a->s, line, sizeof(line), 8000);
    if (r <= 0) { closesocket(a->s); free(a); return 0; }
    char expect[64];
    char* tok = w2u(a->token);
    _snprintf_s(expect, sizeof(expect), _TRUNCATE, "JOIN %s", tok ? tok : "");
    free(tok);
    if (strcmp(line, expect) != 0) {
        nr_send_line(a->s, "ERR auth");
        closesocket(a->s);
        free(a);
        return 0;
    }
    SOCKET g = nr_connect_addr(L"127.0.0.1", a->mc_port, 5000);
    if (g == INVALID_SOCKET) {
        nr_send_line(a->s, "ERR game");
        closesocket(a->s);
        free(a);
        return 0;
    }
    if (!nr_send_line(a->s, "OK")) {
        closesocket(a->s); closesocket(g); free(a); return 0;
    }
    nr_track_socket(a->gen, a->s);
    nr_track_socket(a->gen, g);
    nr_peer_add(a->gen);
    nr_pump(a->s, g, a->gen);
    nr_peer_del(a->gen);
    nr_untrack_socket(a->gen, a->s);
    nr_untrack_socket(a->gen, g);
    closesocket(a->s);
    closesocket(g);
    free(a);
    return 0;
}

static DWORD WINAPI nr_host_direct_thread(LPVOID p) {
    int gen = (int)(INT_PTR)p;
    int mc_port = 0, req = 0;
    wchar_t logs[512];
    EnterCriticalSection(&g_cs);
    mc_port = g_nr.mc_port;
    req = g_nr.req_port;
    wcsncpy_s(logs, 512, g_nr.logs_dir, _TRUNCATE);
    LeaveCriticalSection(&g_cs);

    if (mc_port <= 0) mc_port = nr_wait_log_port(gen, logs, 180000);
    if (!mc_port) {
        if (!nr_stopped(gen)) nr_fail(gen, L"没有检测到 Minecraft 局域网端口。请先在游戏里“对局域网开放”，或在创建房间时手动填写端口。");
        return 0;
    }
    EnterCriticalSection(&g_cs);
    if (gen == g_nr.gen) g_nr.mc_port = mc_port;
    LeaveCriticalSection(&g_cs);

    SOCKET ls;
    int port = 0;
    if (!nr_make_listener(req, &ls, &port)) {
        nr_fail(gen, L"无法监听直连端口 %d（可能被占用或被防火墙拦截）。可改用中转模式。", req > 0 ? req : 25566);
        return 0;
    }
    EnterCriticalSection(&g_cs);
    if (gen == g_nr.gen) g_nr.listener = ls;
    LeaveCriticalSection(&g_cs);

    wchar_t lan[64], ext[64] = L"", code[16], token[32];
    wchar_t invite[NR_INVITE_CAP];
    nr_lan_ip(lan, 64);
    nr_roomcode(code, 6);
    nr_token(token, 10);

    nr_status(gen, L"正在尝试自动映射端口（UPnP）…");
    int mapped = upnp_map_tcp(port, ext, 64);

    if (!mapped || !ext[0]) wcsncpy_s(ext, 64, L"-", _TRUNCATE);
    nr_invite_direct(L"LLcl 房间", ext, lan, port, token, invite, NR_INVITE_CAP);

    EnterCriticalSection(&g_cs);
    if (gen == g_nr.gen) {
        wcsncpy_s(g_nr.room, 32, code, _TRUNCATE);
        wcsncpy_s(g_nr.invite, NR_INVITE_CAP, invite, _TRUNCATE);
        _snwprintf_s(g_nr.address, 256, _TRUNCATE, L"%s:%d", lan, port);
        g_nr.mc_port = mc_port;
    }
    LeaveCriticalSection(&g_cs);

    if (mapped && ext[0] && wcscmp(ext, L"-") != 0)
        nr_status(gen, L"房间已创建（直连）。本机 %s:%d，公网 %s:%d。等待玩家加入…",
                  lan, port, ext, port);
    else
        nr_status(gen, L"房间已创建（直连）。本机 %s:%d。若外网玩家连不上，需要在路由器把 TCP %d 映射到本机，或改用中转模式。",
                  lan, port, port);

    while (!nr_stopped(gen)) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int sr = select(0, &rf, NULL, NULL, &tv);
        if (sr <= 0) continue;
        SOCKET s = accept(ls, NULL, NULL);
        if (s == INVALID_SOCKET) {
            if (nr_stopped(gen)) break;
            continue;
        }
        nr_sock_opts(s);
        nr_conn_arg* a = (nr_conn_arg*)xalloc(sizeof(nr_conn_arg));
        memset(a, 0, sizeof(*a));
        a->gen = gen;
        a->s = s;
        a->mc_port = mc_port;
        wcsncpy_s(a->token, 256, token, _TRUNCATE);
        nr_spawn(nr_direct_host_conn, a, 128 * 1024);
    }
    if (!nr_stopped(gen)) nr_fail(gen, L"直连监听已停止");
    upnp_unmap_tcp(port);
    closesocket(ls);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 直连加入                                                            */
/* ------------------------------------------------------------------ */
static DWORD WINAPI nr_guest_announce_thread(LPVOID p);
static DWORD WINAPI nr_relay_guest_conn(LPVOID p);

static DWORD WINAPI nr_direct_guest_conn(LPVOID p) {
    nr_conn_arg* a = (nr_conn_arg*)p;
    wchar_t f[6][256];
    int n = nr_split(a->token, f, 6);
    if (n < 4) { closesocket(a->s); free(a); return 0; }
    wchar_t endpoints[2][128];
    wcsncpy_s(endpoints[0], 128, f[0], _TRUNCATE);
    wcsncpy_s(endpoints[1], 128, f[1], _TRUNCATE);
    int host_port = _wtoi(f[2]);
    wchar_t token[32];
    wcsncpy_s(token, 32, f[3], _TRUNCATE);
    if (!token[0] || host_port <= 0) { closesocket(a->s); free(a); return 0; }
    SOCKET host = INVALID_SOCKET;
    for (int i = 0; i < 2 && host == INVALID_SOCKET; i++) {
        if (!endpoints[i][0] || wcscmp(endpoints[i], L"-") == 0) continue;
        host = nr_connect_addr(endpoints[i], host_port, 4000);
    }
    if (host == INVALID_SOCKET) {
        nr_status(a->gen, L"连接主机失败：公网端口可能未开放，或主机已停止联机");
        closesocket(a->s);
        free(a);
        return 0;
    }
    char line[NR_LINE], req[NR_LINE];
    char* tk = w2u(token);
    _snprintf_s(req, sizeof(req), _TRUNCATE, "JOIN %s", tk ? tk : "");
    free(tk);
    nr_send_line(host, req);
    int r = nr_recv_line(host, line, sizeof(line), 12000);
    if (r <= 0 || strncmp(line, "OK", 2) != 0) {
        nr_status(a->gen, L"主机拒绝连接（%hs）", line[0] ? line : "timeout");
        closesocket(host);
        closesocket(a->s);
        free(a);
        return 0;
    }
    nr_track_socket(a->gen, a->s);
    nr_track_socket(a->gen, host);
    nr_peer_add(a->gen);
    nr_pump(a->s, host, a->gen);
    nr_peer_del(a->gen);
    nr_untrack_socket(a->gen, a->s);
    nr_untrack_socket(a->gen, host);
    closesocket(a->s);
    closesocket(host);
    free(a);
    return 0;
}

static DWORD WINAPI nr_guest_accept_thread(LPVOID p) {
    int gen = (int)(INT_PTR)p;
    int mode = 0;
    wchar_t code[32], name[64] = L"LLcl 房间";
    EnterCriticalSection(&g_cs);
    mode = g_nr.mode;
    wcsncpy_s(code, 32, g_nr.room, _TRUNCATE);
    LeaveCriticalSection(&g_cs);

    SOCKET ls;
    int port = 0;
    if (!nr_make_listener(0, &ls, &port)) {
        nr_fail(gen, L"无法在本机创建联机代理端口（25566 起均被占用）");
        return 0;
    }
    wchar_t inv[NR_INVITE_CAP] = L"";
    EnterCriticalSection(&g_cs);
    if (gen == g_nr.gen) {
        g_nr.listener = ls;
        g_nr.mc_port = port;
        _snwprintf_s(g_nr.address, 256, _TRUNCATE, L"127.0.0.1:%d", port);
        wcsncpy_s(inv, NR_INVITE_CAP, g_nr.invite, _TRUNCATE);
    }
    LeaveCriticalSection(&g_cs);
    nr_inv ii;
    if (!nr_parse_invite(inv, &ii)) {
        nr_fail(gen, L"邀请码解析失败，请重新复制主机发来的完整邀请码。");
        closesocket(ls);
        return 0;
    }
    wcsncpy_s(name, 64, ii.name, _TRUNCATE);
    wcsncpy_s(code, 32, ii.code, _TRUNCATE);

    {
        typedef struct { int gen; int port; wchar_t name[64]; } ann_arg;
        ann_arg* aa = (ann_arg*)xalloc(sizeof(ann_arg));
        memset(aa, 0, sizeof(*aa));
        aa->gen = gen;
        aa->port = port;
        wcsncpy_s(aa->name, 64, name, _TRUNCATE);
        nr_spawn(nr_guest_announce_thread, aa, 64 * 1024);
    }

    if (mode == NET_MODE_DIRECT) {
        nr_status(gen, L"已加入房间。打开游戏 → 多人游戏，局域网列表会出现“%s”；也可直连 127.0.0.1:%d。",
                  name, port);
    } else {
        nr_status(gen, L"正在通过中转服务器加入房间 %s…本机代理 127.0.0.1:%d（游戏里会显示局域网房间）。",
                  code, port);
    }

    while (!nr_stopped(gen)) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int sr = select(0, &rf, NULL, NULL, &tv);
        if (sr <= 0) continue;
        SOCKET s = accept(ls, NULL, NULL);
        if (s == INVALID_SOCKET) {
            if (nr_stopped(gen)) break;
            continue;
        }
        nr_sock_opts(s);
        nr_conn_arg* a = (nr_conn_arg*)xalloc(sizeof(nr_conn_arg));
        memset(a, 0, sizeof(*a));
        a->gen = gen;
        a->s = s;
        if (mode == NET_MODE_DIRECT) {
            _snwprintf_s(a->token, 256, _TRUNCATE, L"%.60s|%.60s|%d|%.28s",
                         ii.pub, ii.lan, ii.port, ii.token);
            nr_spawn(nr_direct_guest_conn, a, 128 * 1024);
        } else {
            _snwprintf_s(a->token, 256, _TRUNCATE, L"%.60s|%d|%.8s|%.28s",
                         ii.relay_host, ii.relay_port, ii.code, ii.token);
            nr_spawn(nr_relay_guest_conn, a, 128 * 1024);
        }
    }
    if (!nr_stopped(gen)) nr_fail(gen, L"联机代理已停止");
    closesocket(ls);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Minecraft LAN 公告                                                  */
/* ------------------------------------------------------------------ */
typedef struct { int gen; int port; wchar_t name[64]; } nr_ann_arg;

static DWORD WINAPI nr_guest_announce_thread(LPVOID p) {
    nr_ann_arg* a = (nr_ann_arg*)p;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { free(a); return 0; }
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
    int loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loop, sizeof(loop));
    unsigned char ttl = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));

    char* n8 = w2u(a->name[0] ? a->name : L"LLcl 房间");
    if (!n8) n8 = udup("LLcl");
    for (char* q = n8; *q; q++) if (*q == '[' || *q == ']' || *q == '\r' || *q == '\n') *q = ' ';
    char pkt[512];
    _snprintf_s(pkt, sizeof(pkt), _TRUNCATE, "[MOTD]%s - LLcl[/MOTD][AD]%d[/AD]", n8, a->port);
    free(n8);

    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(4445);
    to.sin_addr.s_addr = inet_addr("224.0.2.60");
    sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(4445);
    local.sin_addr.s_addr = inet_addr("127.0.0.1");

    while (!nr_stopped(a->gen)) {
        sendto(s, pkt, (int)strlen(pkt), 0, (sockaddr*)&to, sizeof(to));
        sendto(s, pkt, (int)strlen(pkt), 0, (sockaddr*)&local, sizeof(local));
        for (int i = 0; i < 6 && !nr_stopped(a->gen); i++) Sleep(250);
    }
    closesocket(s);
    free(a);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 中转服务器                                                          */
/* ------------------------------------------------------------------ */
typedef struct {
    int used;
    int dead;
    int gen;
    char code[16];
    char token[24];
    SOCKET host;
    struct {
        int used;
        int ready;
        char sid[16];
        SOCKET guest;
        SOCKET bind;
        HANDLE ev;
    } peers[NR_RELAY_PEERS];
} relay_room;

static relay_room g_rr[NR_RELAY_ROOMS];
static CRITICAL_SECTION g_rcs;
static LONG g_rcs_ready = 0;
static volatile LONG g_relay_run = 0;
static SOCKET g_relay_listen = INVALID_SOCKET;

static void nr_rcs_init(void) {
    if (InterlockedCompareExchange(&g_rcs_ready, 1, 0) == 0) {
        InitializeCriticalSection(&g_rcs);
        InterlockedExchange(&g_rcs_ready, 2);
    } else {
        while (g_rcs_ready == 1) Sleep(0);
    }
}

static int relay_start(int port, int* actual) {
    if (!nr_wsa_init()) return 0;
    nr_rcs_init();
    SOCKET ls;
    int p = 0;
    if (!nr_make_listener(port, &ls, &p)) return 0;
    g_relay_listen = ls;
    *actual = p;
    return 1;
}

static int relay_find_room(const char* code) {
    for (int i = 0; i < NR_RELAY_ROOMS; i++)
        if (g_rr[i].used && !g_rr[i].dead && strcmp(g_rr[i].code, code) == 0) return i;
    return -1;
}

static void relay_room_release(int ri) {
    relay_room* r = &g_rr[ri];
    /* 主机控制连接由 host 线程自己 closesocket；这里只 shutdown，
       避免同一句柄被关闭两次后又被系统复用。 */
    if (r->host != INVALID_SOCKET) {
        shutdown(r->host, SD_BOTH);
        r->host = INVALID_SOCKET;
    }
    r->dead = 1;
    int any = 0;
    for (int i = 0; i < NR_RELAY_PEERS; i++) {
        if (!r->peers[i].used) continue;
        any = 1;
        if (r->peers[i].guest != INVALID_SOCKET) {
            closesocket(r->peers[i].guest);
            r->peers[i].guest = INVALID_SOCKET;
        }
        if (r->peers[i].bind != INVALID_SOCKET) {
            closesocket(r->peers[i].bind);
            r->peers[i].bind = INVALID_SOCKET;
        }
        if (r->peers[i].ev) SetEvent(r->peers[i].ev);
    }
    if (!any) memset(r, 0, sizeof(*r));
}

static void relay_peer_free(int ri, int pi, int gen) {
    relay_room* r = &g_rr[ri];
    if (r->gen != gen) return;
    if (r->peers[pi].guest != INVALID_SOCKET) closesocket(r->peers[pi].guest);
    if (r->peers[pi].bind != INVALID_SOCKET) closesocket(r->peers[pi].bind);
    if (r->peers[pi].ev) CloseHandle(r->peers[pi].ev);
    memset(&r->peers[pi], 0, sizeof(r->peers[pi]));
    r->peers[pi].guest = INVALID_SOCKET;
    r->peers[pi].bind = INVALID_SOCKET;
    if (r->dead) {
        int any = 0;
        for (int i = 0; i < NR_RELAY_PEERS; i++) if (r->peers[i].used) any = 1;
        if (!any) memset(r, 0, sizeof(*r));
    }
}

static DWORD WINAPI relay_conn_thread(LPVOID p) {
    SOCKET s = (SOCKET)(uintptr_t)p;
    nr_sock_opts(s);
    char line[NR_LINE];
    int n = nr_recv_line(s, line, sizeof(line), 8000);
    if (n <= 0) { closesocket(s); return 0; }

    char cmd[16] = "", a1[128] = "", a2[64] = "";
    if (sscanf_s(line, "%15s %127s %63s", cmd, (unsigned)sizeof(cmd), a1, (unsigned)sizeof(a1),
                 a2, (unsigned)sizeof(a2)) < 2) {
        nr_send_line(s, "ERR proto");
        closesocket(s);
        return 0;
    }

    if (strcmp(cmd, "HOST") == 0) {
        if (!a1[0] || !a2[0]) { nr_send_line(s, "ERR args"); closesocket(s); return 0; }
        int ri = -1;
        EnterCriticalSection(&g_rcs);
        if (relay_find_room(a1) >= 0) {
            LeaveCriticalSection(&g_rcs);
            nr_send_line(s, "ERR exists");
            closesocket(s);
            return 0;
        }
        for (int i = 0; i < NR_RELAY_ROOMS; i++)
            if (!g_rr[i].used) { ri = i; break; }
        if (ri < 0) {
            LeaveCriticalSection(&g_rcs);
            nr_send_line(s, "ERR full");
            closesocket(s);
            return 0;
        }
        memset(&g_rr[ri], 0, sizeof(g_rr[ri]));
        g_rr[ri].used = 1;
        g_rr[ri].gen++;
        g_rr[ri].host = s;
        for (int i = 0; i < NR_RELAY_PEERS; i++) {
            g_rr[ri].peers[i].guest = INVALID_SOCKET;
            g_rr[ri].peers[i].bind = INVALID_SOCKET;
        }
        strncpy_s(g_rr[ri].code, sizeof(g_rr[ri].code), a1, _TRUNCATE);
        strncpy_s(g_rr[ri].token, sizeof(g_rr[ri].token), a2, _TRUNCATE);
        LeaveCriticalSection(&g_rcs);
        nr_send_line(s, "OK");

        for (;;) {
            int r = nr_recv_line(s, line, sizeof(line), 30000);
            if (r == -2) { nr_send_line(s, "PING"); continue; }
            if (r <= 0) break;
            if (strcmp(line, "PING") == 0) nr_send_line(s, "PONG");
        }
        EnterCriticalSection(&g_rcs);
        if (g_rr[ri].used && g_rr[ri].host == s) relay_room_release(ri);
        LeaveCriticalSection(&g_rcs);
        closesocket(s);
        return 0;
    }

    if (strcmp(cmd, "JOIN") == 0) {
        int ri = -1, pi = -1;
        EnterCriticalSection(&g_rcs);
        ri = relay_find_room(a1);
        if (ri < 0 || !a2[0] || strcmp(g_rr[ri].token, a2) != 0) {
            LeaveCriticalSection(&g_rcs);
            nr_send_line(s, "ERR auth");
            closesocket(s);
            return 0;
        }
        if (ri >= 0) {
            for (int i = 0; i < NR_RELAY_PEERS; i++)
                if (!g_rr[ri].peers[i].used) { pi = i; break; }
        }
        if (ri < 0 || pi < 0) {
            LeaveCriticalSection(&g_rcs);
            nr_send_line(s, "ERR noroom");
            closesocket(s);
            return 0;
        }
        int rg = g_rr[ri].gen;
        char sid[16];
        _snprintf_s(sid, sizeof(sid), _TRUNCATE, "%08x", (unsigned)nr_rand());
        strncpy_s(g_rr[ri].peers[pi].sid, sizeof(g_rr[ri].peers[pi].sid), sid, _TRUNCATE);
        g_rr[ri].peers[pi].used = 1;
        g_rr[ri].peers[pi].ready = 0;
        g_rr[ri].peers[pi].guest = s;
        g_rr[ri].peers[pi].bind = INVALID_SOCKET;
        g_rr[ri].peers[pi].ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        SOCKET host = g_rr[ri].host;
        LeaveCriticalSection(&g_rcs);

        char msg[64];
        _snprintf_s(msg, sizeof(msg), _TRUNCATE, "PEER %s", sid);
        if (!nr_send_line(host, msg)) {
            EnterCriticalSection(&g_rcs);
            relay_peer_free(ri, pi, rg);
            LeaveCriticalSection(&g_rcs);
            closesocket(s);
            return 0;
        }
        nr_send_line(s, "WAIT");
        int ready = 0;
        HANDLE ev = NULL;
        EnterCriticalSection(&g_rcs);
        if (g_rr[ri].gen == rg) ev = g_rr[ri].peers[pi].ev;
        LeaveCriticalSection(&g_rcs);
        if (ev && WaitForSingleObject(ev, 20000) == WAIT_OBJECT_0) {
            EnterCriticalSection(&g_rcs);
            if (g_rr[ri].gen == rg && g_rr[ri].peers[pi].used && g_rr[ri].peers[pi].ready == 1)
                ready = 1;
            LeaveCriticalSection(&g_rcs);
        }
        if (!ready) {
            nr_send_line(s, "ERR host");
            EnterCriticalSection(&g_rcs);
            relay_peer_free(ri, pi, rg);
            LeaveCriticalSection(&g_rcs);
            return 0;
        }
        SOCKET bind_s;
        EnterCriticalSection(&g_rcs);
        bind_s = (g_rr[ri].gen == rg) ? g_rr[ri].peers[pi].bind : INVALID_SOCKET;
        LeaveCriticalSection(&g_rcs);
        if (bind_s == INVALID_SOCKET) {
            nr_send_line(s, "ERR host");
            EnterCriticalSection(&g_rcs);
            relay_peer_free(ri, pi, rg);
            LeaveCriticalSection(&g_rcs);
            return 0;
        }
        nr_send_line(s, "READY");
        for (;;) {
            if (!g_relay_run) break;
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(s, &rf);
            FD_SET(bind_s, &rf);
            timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            int sr = select(0, &rf, NULL, NULL, &tv);
            if (sr <= 0) continue;
            char buf[NR_BUF];
            int closed = 0;
            if (FD_ISSET(s, &rf)) {
                int r = recv(s, buf, sizeof(buf), 0);
                if (r > 0) { if (!nr_send_all(bind_s, buf, r)) closed = 1; }
                else if (r == 0) { shutdown(bind_s, SD_SEND); closed = 1; }
                else if (WSAGetLastError() != WSAETIMEDOUT) closed = 1;
            }
            if (!closed && FD_ISSET(bind_s, &rf)) {
                int r = recv(bind_s, buf, sizeof(buf), 0);
                if (r > 0) { if (!nr_send_all(s, buf, r)) closed = 1; }
                else if (r == 0) { shutdown(s, SD_SEND); closed = 1; }
                else if (WSAGetLastError() != WSAETIMEDOUT) closed = 1;
            }
            if (closed) break;
        }
        EnterCriticalSection(&g_rcs);
        relay_peer_free(ri, pi, rg);
        LeaveCriticalSection(&g_rcs);
        return 0;
    }

    if (strcmp(cmd, "BIND") == 0) {
        int ri = -1, pi = -1, rg = 0;
        EnterCriticalSection(&g_rcs);
        for (int i = 0; i < NR_RELAY_ROOMS && ri < 0; i++) {
            if (!g_rr[i].used) continue;
            for (int j = 0; j < NR_RELAY_PEERS; j++) {
                if (g_rr[i].peers[j].used && strcmp(g_rr[i].peers[j].sid, a1) == 0) {
                    ri = i; pi = j; rg = g_rr[i].gen;
                    break;
                }
            }
        }
        if (ri < 0 || !a2[0] || strcmp(g_rr[ri].token, a2) != 0) {
            LeaveCriticalSection(&g_rcs);
            nr_send_line(s, "ERR auth");
            closesocket(s);
            return 0;
        }
        g_rr[ri].peers[pi].bind = s;
        LeaveCriticalSection(&g_rcs);
        nr_send_line(s, "OK");
        int r = nr_recv_line(s, line, sizeof(line), 10000);
        int go = (r > 0 && strncmp(line, "GO", 2) == 0);
        int fail = (r > 0 && strncmp(line, "FAIL", 4) == 0);
        EnterCriticalSection(&g_rcs);
        if (g_rr[ri].gen == rg && g_rr[ri].peers[pi].used) {
            g_rr[ri].peers[pi].ready = go ? 1 : -1;
            if (g_rr[ri].peers[pi].ev) SetEvent(g_rr[ri].peers[pi].ev);
        }
        LeaveCriticalSection(&g_rcs);
        if (!go) {
            if (!fail) nr_send_line(s, "FAIL");
            closesocket(s);
            EnterCriticalSection(&g_rcs);
            if (g_rr[ri].gen == rg && g_rr[ri].peers[pi].used && g_rr[ri].peers[pi].bind == s)
                g_rr[ri].peers[pi].bind = INVALID_SOCKET;
            LeaveCriticalSection(&g_rcs);
        }
        return 0;
    }

    nr_send_line(s, "ERR cmd");
    closesocket(s);
    return 0;
}

static DWORD WINAPI relay_accept_thread(LPVOID p) {
    (void)p;
    while (g_relay_run) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(g_relay_listen, &rf);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int sr = select(0, &rf, NULL, NULL, &tv);
        if (sr <= 0) continue;
        SOCKET s = accept(g_relay_listen, NULL, NULL);
        if (s == INVALID_SOCKET) continue;
        nr_spawn(relay_conn_thread, (void*)(uintptr_t)s, 128 * 1024);
    }
    return 0;
}

static void relay_shutdown(void) {
    InterlockedExchange(&g_relay_run, 0);
    if (g_relay_listen != INVALID_SOCKET) {
        closesocket(g_relay_listen);
        g_relay_listen = INVALID_SOCKET;
    }
    EnterCriticalSection(&g_rcs);
    for (int i = 0; i < NR_RELAY_ROOMS; i++)
        if (g_rr[i].used) relay_room_release(i);
    LeaveCriticalSection(&g_rcs);
}

int net_relay_run(int port) {
    int actual = 0;
    if (!relay_start(port, &actual)) {
        cli_print(L"无法启动中转服务器：端口被占用或 Winsock 初始化失败。\n");
        return 1;
    }
    InterlockedExchange(&g_relay_run, 1);
    HANDLE h = CreateThread(NULL, 128 * 1024, relay_accept_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
    cli_print(L"LLcl 中转服务器已启动。\n");
    cli_printf(L"监听: 0.0.0.0:%d（TCP）\n", actual);
    cli_print(L"主机创建房间时填写该地址；玩家加入时邀请码里会带中转地址。\n");
    cli_print(L"按 Ctrl+C 或关闭控制台结束。\n");
    for (;;) {
        Sleep(1000);
        if (!g_relay_run) break;
    }
    relay_shutdown();
    return 0;
}

/* ------------------------------------------------------------------ */
/* 中转主机                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    int gen;
    wchar_t host[128];
    int port;
    int mc_port;
    char sid[16];
    char token[32];
} nr_peer_arg;

static DWORD WINAPI nr_relay_host_peer(LPVOID p) {
    nr_peer_arg* a = (nr_peer_arg*)p;
    SOCKET r = nr_connect_addr(a->host, a->port, 8000);
    if (r == INVALID_SOCKET) { free(a); return 0; }
    char line[NR_LINE], req[NR_LINE];
    _snprintf_s(req, sizeof(req), _TRUNCATE, "BIND %s %s", a->sid, a->token);
    if (!nr_send_line(r, req) || nr_recv_line(r, line, sizeof(line), 8000) <= 0 ||
        strncmp(line, "OK", 2) != 0) {
        closesocket(r);
        free(a);
        return 0;
    }
    SOCKET g = nr_connect_addr(L"127.0.0.1", a->mc_port, 5000);
    if (g == INVALID_SOCKET) {
        nr_send_line(r, "FAIL game");
        closesocket(r);
        nr_log(a->gen, L"玩家连接失败：Minecraft 局域网端口 %d 无法连接", a->mc_port);
        free(a);
        return 0;
    }
    if (!nr_send_line(r, "GO")) {
        closesocket(r); closesocket(g); free(a); return 0;
    }
    nr_track_socket(a->gen, r);
    nr_track_socket(a->gen, g);
    nr_peer_add(a->gen);
    nr_pump(r, g, a->gen);
    nr_peer_del(a->gen);
    nr_untrack_socket(a->gen, r);
    nr_untrack_socket(a->gen, g);
    closesocket(r);
    closesocket(g);
    free(a);
    return 0;
}

static DWORD WINAPI nr_host_relay_thread(LPVOID p) {
    int gen = (int)(INT_PTR)p;
    int mc_port = 0;
    wchar_t relay[256], logs[512];
    EnterCriticalSection(&g_cs);
    mc_port = g_nr.mc_port;
    wcsncpy_s(relay, 256, g_nr.relay, _TRUNCATE);
    wcsncpy_s(logs, 512, g_nr.logs_dir, _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    if (mc_port <= 0) mc_port = nr_wait_log_port(gen, logs, 180000);
    if (!mc_port) {
        if (!nr_stopped(gen)) nr_fail(gen, L"没有检测到 Minecraft 局域网端口。请先“对局域网开放”或手动填写端口。");
        return 0;
    }
    wchar_t rhost[128];
    int rport = 0;
    if (!nr_parse_hostport(relay, rhost, 128, &rport)) {
        nr_fail(gen, L"中转服务器地址无效：%s", relay);
        return 0;
    }
    SOCKET ctl = nr_connect_addr(rhost, rport, 10000);
    if (ctl == INVALID_SOCKET) {
        nr_fail(gen, L"无法连接中转服务器 %s:%d。请确认服务器已启动、端口已开放。", rhost, rport);
        return 0;
    }
    wchar_t code[16], token[32];
    nr_roomcode(code, 6);
    nr_token(token, 10);
    char* c8 = w2u(code);
    char* t8 = w2u(token);
    char reqline[128];
    _snprintf_s(reqline, sizeof(reqline), _TRUNCATE, "HOST %s %s", c8 ? c8 : "", t8 ? t8 : "");
    free(c8); free(t8);
    char line[NR_LINE];
    if (!nr_send_line(ctl, reqline) || nr_recv_line(ctl, line, sizeof(line), 10000) <= 0 ||
        strncmp(line, "OK", 2) != 0) {
        nr_fail(gen, L"中转服务器拒绝创建房间（%hs）", line[0] ? line : "timeout");
        closesocket(ctl);
        return 0;
    }
    EnterCriticalSection(&g_cs);
    if (gen == g_nr.gen) {
        g_nr.control = ctl;
        wcsncpy_s(g_nr.room, 32, code, _TRUNCATE);
        g_nr.mc_port = mc_port;
        wchar_t inv[NR_INVITE_CAP];
        nr_invite_relay(L"LLcl 房间", rhost, rport, code, token, inv, NR_INVITE_CAP);
        wcsncpy_s(g_nr.invite, NR_INVITE_CAP, inv, _TRUNCATE);
        _snwprintf_s(g_nr.address, 256, _TRUNCATE, L"%s:%d", rhost, rport);
    }
    LeaveCriticalSection(&g_cs);
    nr_status(gen, L"房间已创建（中转 %s:%d），房间码 %s。等待玩家加入…", rhost, rport, code);

    while (!nr_stopped(gen)) {
        int r = nr_recv_line(ctl, line, sizeof(line), 30000);
        if (r == -2) { nr_send_line(ctl, "PING"); continue; }
        if (r <= 0) break;
        if (strncmp(line, "PEER ", 5) == 0) {
            nr_peer_arg* a = (nr_peer_arg*)xalloc(sizeof(nr_peer_arg));
            memset(a, 0, sizeof(*a));
            a->gen = gen;
            wcsncpy_s(a->host, 128, rhost, _TRUNCATE);
            a->port = rport;
            a->mc_port = mc_port;
            strncpy_s(a->sid, sizeof(a->sid), line + 5, _TRUNCATE);
            {
                char* tt = w2u(token);
                if (tt) { strncpy_s(a->token, sizeof(a->token), tt, _TRUNCATE); free(tt); }
            }
            nr_spawn(nr_relay_host_peer, a, 128 * 1024);
        } else if (strcmp(line, "PING") == 0) {
            nr_send_line(ctl, "PONG");
        }
    }
    if (!nr_stopped(gen)) nr_fail(gen, L"中转服务器连接已断开，房间已关闭。");
    closesocket(ctl);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 中转加入                                                            */
/* ------------------------------------------------------------------ */
static DWORD WINAPI nr_relay_guest_conn(LPVOID p) {
    nr_conn_arg* a = (nr_conn_arg*)p;
    wchar_t f[6][256];
    int n = nr_split(a->token, f, 6);
    if (n < 4) { closesocket(a->s); free(a); return 0; }
    int rport = _wtoi(f[1]);
    SOCKET r = nr_connect_addr(f[0], rport, 10000);
    if (r == INVALID_SOCKET) {
        nr_status(a->gen, L"无法连接中转服务器 %s:%d", f[0], rport);
        closesocket(a->s);
        free(a);
        return 0;
    }
    char req[256];
    char* c8 = w2u(f[2]);
    char* t8 = w2u(f[3]);
    _snprintf_s(req, sizeof(req), _TRUNCATE, "JOIN %s %s", c8 ? c8 : "", t8 ? t8 : "");
    free(c8); free(t8);
    if (!nr_send_line(r, req)) {
        closesocket(r); closesocket(a->s); free(a); return 0;
    }
    char line[NR_LINE];
    int x = nr_recv_line(r, line, sizeof(line), 8000);
    if (x <= 0 || strncmp(line, "WAIT", 4) != 0) {
        nr_status(a->gen, L"加入中转房间失败（%hs）", line[0] ? line : "timeout");
        closesocket(r); closesocket(a->s); free(a); return 0;
    }
    x = nr_recv_line(r, line, sizeof(line), 25000);
    if (x <= 0 || strncmp(line, "READY", 5) != 0) {
        nr_status(a->gen, L"房间未就绪：%hs。主机可能没有打开游戏或已离开。",
                  line[0] ? line : "host timeout");
        closesocket(r); closesocket(a->s); free(a); return 0;
    }
    nr_status(a->gen, L"已连接到主机，正在转发游戏数据…");
    nr_track_socket(a->gen, a->s);
    nr_track_socket(a->gen, r);
    nr_peer_add(a->gen);
    nr_pump(a->s, r, a->gen);
    nr_peer_del(a->gen);
    nr_untrack_socket(a->gen, a->s);
    nr_untrack_socket(a->gen, r);
    closesocket(a->s);
    closesocket(r);
    free(a);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 启动 / 停止                                                         */
/* ------------------------------------------------------------------ */
int net_create_host(int mode, const wchar_t* relay_addr, int mc_port, int listen_port,
                    const wchar_t* logs_dir, wchar_t* err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!nr_wsa_init()) { if (err) wcsncpy_s(err, errlen, L"Winsock 初始化失败", _TRUNCATE); return 0; }
    nr_cs_init();
    if (mode != NET_MODE_DIRECT && mode != NET_MODE_RELAY) {
        if (err) wcsncpy_s(err, errlen, L"未知联机模式", _TRUNCATE);
        return 0;
    }
    if (mode == NET_MODE_RELAY && (!relay_addr || !relay_addr[0])) {
        if (err) wcsncpy_s(err, errlen, L"中转模式需要填写中转服务器地址，例如 1.2.3.4:25566", _TRUNCATE);
        return 0;
    }
    EnterCriticalSection(&g_cs);
    if (g_nr.active) {
        LeaveCriticalSection(&g_cs);
        if (err) wcsncpy_s(err, errlen, L"已有联机房间在运行，请先停止", _TRUNCATE);
        return 0;
    }
    g_nr.gen++;
    int gen = g_nr.gen;
    g_nr.stop = 0;
    g_nr.active = 1;
    g_nr.is_host = 1;
    g_nr.mode = mode;
    g_nr.peers = 0;
    g_nr.mc_port = mc_port;
    g_nr.req_port = listen_port;
    g_nr.listener = INVALID_SOCKET;
    g_nr.control = INVALID_SOCKET;
    g_nr.nsocks = 0;
    g_nr.room[0] = 0;
    g_nr.invite[0] = 0;
    g_nr.address[0] = 0;
    wcsncpy_s(g_nr.relay, 256, relay_addr ? relay_addr : L"", _TRUNCATE);
    wcsncpy_s(g_nr.logs_dir, 512, logs_dir ? logs_dir : L"", _TRUNCATE);
    wcsncpy_s(g_nr.status, 512, L"正在创建房间…", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    nr_emit_raw(NET_EV_STATUS, L"正在创建房间…");
    nr_spawn(mode == NET_MODE_DIRECT ? nr_host_direct_thread : nr_host_relay_thread,
             (void*)(INT_PTR)gen, 128 * 1024);
    return 1;
}

int net_join(const wchar_t* invite, wchar_t* err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!nr_wsa_init()) { if (err) wcsncpy_s(err, errlen, L"Winsock 初始化失败", _TRUNCATE); return 0; }
    nr_cs_init();
    nr_inv inv;
    if (!invite || !nr_parse_invite(invite, &inv)) {
        if (err) wcsncpy_s(err, errlen, L"邀请码无效。请让主机复制完整的“LLCL1|…”邀请码。", _TRUNCATE);
        return 0;
    }
    EnterCriticalSection(&g_cs);
    if (g_nr.active) {
        LeaveCriticalSection(&g_cs);
        if (err) wcsncpy_s(err, errlen, L"已有联机房间在运行，请先停止", _TRUNCATE);
        return 0;
    }
    g_nr.gen++;
    int gen = g_nr.gen;
    g_nr.stop = 0;
    g_nr.active = 1;
    g_nr.is_host = 0;
    g_nr.mode = inv.mode;
    g_nr.peers = 0;
    g_nr.mc_port = 0;
    g_nr.req_port = 0;
    g_nr.listener = INVALID_SOCKET;
    g_nr.control = INVALID_SOCKET;
    g_nr.nsocks = 0;
    g_nr.room[0] = 0;
    g_nr.address[0] = 0;
    wcsncpy_s(g_nr.invite, NR_INVITE_CAP, invite, _TRUNCATE);
    wcsncpy_s(g_nr.relay, 256, inv.relay_host, _TRUNCATE);
    wcsncpy_s(g_nr.status, 512, L"正在加入房间…", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    nr_emit_raw(NET_EV_STATUS, L"正在加入房间…");
    nr_spawn(nr_guest_accept_thread, (void*)(INT_PTR)gen, 128 * 1024);
    return 1;
}

void net_stop(void) {
    nr_cs_init();
    SOCKET shut[NR_MAX_SOCKS + 2];
    int ns = 0;
    EnterCriticalSection(&g_cs);
    int was = g_nr.active;
    g_nr.stop = 1;
    g_nr.active = 0;
    g_nr.gen++;
    /* 监听 socket 由工作线程自己关闭（select 每 1 秒检查 stop），
       这里只 shutdown 控制/数据连接，避免和线程里的 closesocket 重复关闭。 */
    if (g_nr.control != INVALID_SOCKET) shut[ns++] = g_nr.control;
    for (int i = 0; i < g_nr.nsocks; i++) shut[ns++] = g_nr.socks[i];
    g_nr.listener = INVALID_SOCKET;
    g_nr.control = INVALID_SOCKET;
    g_nr.nsocks = 0;
    wcsncpy_s(g_nr.status, 512, L"未联机", _TRUNCATE);
    LeaveCriticalSection(&g_cs);
    for (int i = 0; i < ns; i++) shutdown(shut[i], SD_BOTH);
    if (was) nr_emit_raw(NET_EV_STOPPED, L"联机已停止");
}

int net_active(void) {
    int a;
    nr_cs_init();
    EnterCriticalSection(&g_cs);
    a = g_nr.active;
    LeaveCriticalSection(&g_cs);
    return a;
}

void net_get_status(net_status* st) {
    if (!st) return;
    nr_cs_init();
    memset(st, 0, sizeof(*st));
    EnterCriticalSection(&g_cs);
    st->active = g_nr.active;
    st->is_host = g_nr.is_host;
    st->mode = g_nr.mode;
    st->peers = g_nr.peers;
    st->mc_port = g_nr.mc_port;
    wcsncpy_s(st->room, 32, g_nr.room, _TRUNCATE);
    wcsncpy_s(st->invite, 1024, g_nr.invite, _TRUNCATE);
    wcsncpy_s(st->address, 256, g_nr.address, _TRUNCATE);
    wcsncpy_s(st->relay, 256, g_nr.relay, _TRUNCATE);
    wcsncpy_s(st->status, 512, g_nr.status, _TRUNCATE);
    LeaveCriticalSection(&g_cs);
}

/* ------------------------------------------------------------------ */
/* 回环自检                                                            */
/* ------------------------------------------------------------------ */
static volatile LONG g_test_mc_stop = 0;
static SOCKET g_test_mc_sock = INVALID_SOCKET;

static DWORD WINAPI test_mc_thread(LPVOID p) {
    (void)p;
    while (!g_test_mc_stop) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(g_test_mc_sock, &rf);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        if (select(0, &rf, NULL, NULL, &tv) <= 0) continue;
        SOCKET s = accept(g_test_mc_sock, NULL, NULL);
        if (s == INVALID_SOCKET) continue;
        char buf[64];
        int n = recv(s, buf, sizeof(buf), 0);
        if (n > 0) send(s, "PONG\n", 5, 0);
        closesocket(s);
    }
    return 0;
}

static int test_wait_invite(wchar_t* out, int cap, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        net_status st;
        net_get_status(&st);
        if (st.invite[0] && st.active) {
            wcsncpy_s(out, cap, st.invite, _TRUNCATE);
            return 1;
        }
        if (!st.active) return 0;
        Sleep(50);
        waited += 50;
    }
    return 0;
}

static int test_wait_address(wchar_t* out, int cap, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        net_status st;
        net_get_status(&st);
        if (st.address[0] && wcsstr(st.address, L"127.0.0.1:") && st.mc_port > 0) {
            wcsncpy_s(out, cap, st.address, _TRUNCATE);
            return st.mc_port;
        }
        if (!st.active) return 0;
        Sleep(50);
        waited += 50;
    }
    return 0;
}

static int test_roundtrip(int port) {
    SOCKET s = nr_connect_addr(L"127.0.0.1", port, 6000);
    if (s == INVALID_SOCKET) return 0;
    int ok = nr_send_all(s, "PING\n", 5);
    char buf[64] = "";
    if (ok) {
        int r = recv(s, buf, sizeof(buf) - 1, 0);
        if (r > 0) buf[r] = 0;
        ok = (r > 0 && strncmp(buf, "PONG", 4) == 0);
    }
    closesocket(s);
    return ok;
}

static int test_raw_relay(int relay_port, int mc_port) {
    (void)mc_port;
    wchar_t code[16], token[32];
    nr_roomcode(code, 6);
    nr_token(token, 10);
    char* c8 = w2u(code);
    char* t8 = w2u(token);
    char line[NR_LINE], req[NR_LINE];

    SOCKET host = nr_connect_addr(L"127.0.0.1", relay_port, 4000);
    if (host == INVALID_SOCKET) { free(c8); free(t8); return 0; }
    _snprintf_s(req, sizeof(req), _TRUNCATE, "HOST %s %s", c8, t8);
    nr_send_line(host, req);
    int r = nr_recv_line(host, line, sizeof(line), 4000);
    if (r <= 0 || strncmp(line, "OK", 2) != 0) {
        closesocket(host); free(c8); free(t8); return 0;
    }

    SOCKET guest = nr_connect_addr(L"127.0.0.1", relay_port, 4000);
    if (guest == INVALID_SOCKET) { closesocket(host); free(c8); free(t8); return 0; }
    _snprintf_s(req, sizeof(req), _TRUNCATE, "JOIN %s %s", c8, t8);
    nr_send_line(guest, req);
    r = nr_recv_line(guest, line, sizeof(line), 4000);
    int ok = (r > 0 && strncmp(line, "WAIT", 4) == 0);

    char sid[32] = "";
    if (ok) {
        r = nr_recv_line(host, line, sizeof(line), 4000);
        if (r > 0 && strncmp(line, "PEER ", 5) == 0) strncpy_s(sid, sizeof(sid), line + 5, _TRUNCATE);
        else ok = 0;
    }
    SOCKET bind_s = INVALID_SOCKET;
    if (ok) {
        bind_s = nr_connect_addr(L"127.0.0.1", relay_port, 4000);
        if (bind_s == INVALID_SOCKET) ok = 0;
    }
    if (ok) {
        _snprintf_s(req, sizeof(req), _TRUNCATE, "BIND %s %s", sid, t8);
        nr_send_line(bind_s, req);
        r = nr_recv_line(bind_s, line, sizeof(line), 4000);
        if (r <= 0 || strncmp(line, "OK", 2) != 0) ok = 0;
    }
    if (ok) {
        nr_send_line(bind_s, "GO");
        r = nr_recv_line(guest, line, sizeof(line), 4000);
        if (r <= 0 || strncmp(line, "READY", 5) != 0) ok = 0;
    }
    if (ok) {
        nr_send_all(guest, "PING\n", 5);
        r = nr_recv_line(bind_s, line, sizeof(line), 4000);
        if (r <= 0 || strncmp(line, "PING", 4) != 0) ok = 0;
    }
    if (ok) {
        nr_send_all(bind_s, "PONG\n", 5);
        r = nr_recv_line(guest, line, sizeof(line), 4000);
        if (r <= 0 || strncmp(line, "PONG", 4) != 0) ok = 0;
    }
    if (bind_s != INVALID_SOCKET) closesocket(bind_s);
    closesocket(guest);
    closesocket(host);
    free(c8); free(t8);
    return ok;
}

typedef struct { SOCKET ls; int mc_port; wchar_t token[32]; } test_host_arg;

static DWORD WINAPI test_manual_host_thread(LPVOID p) {
    test_host_arg* h = (test_host_arg*)p;
    SOCKET s = accept(h->ls, NULL, NULL);
    if (s != INVALID_SOCKET) {
        nr_sock_opts(s);
        nr_conn_arg* a = (nr_conn_arg*)xalloc(sizeof(nr_conn_arg));
        memset(a, 0, sizeof(*a));
        EnterCriticalSection(&g_cs);
        a->gen = g_nr.gen;   /* 测试用：与加入方同一代号，pump 才会运行 */
        LeaveCriticalSection(&g_cs);
        a->s = s;
        a->mc_port = h->mc_port;
        wcsncpy_s(a->token, 256, h->token, _TRUNCATE);
        nr_direct_host_conn(a);
    }
    closesocket(h->ls);
    free(h);
    return 0;
}

static int test_direct_engine(int mc_port) {
    wchar_t err[512] = L"";
    if (!net_create_host(NET_MODE_DIRECT, NULL, mc_port, 0, NULL, err, 512)) {
        cli_printf(L"    direct create failed: %s\n", err);
        return 0;
    }
    wchar_t invite[1024] = L"";
    if (!test_wait_invite(invite, 1024, 20000)) {
        net_status st;
        net_get_status(&st);
        cli_printf(L"    direct wait invite failed: %s\n", st.status);
        net_stop();
        return 0;
    }
    nr_inv inv;
    if (!nr_parse_invite(invite, &inv)) { cli_print(L"    direct invite parse failed\n"); net_stop(); return 0; }
    SOCKET s = nr_connect_addr(inv.lan, inv.port, 4000);
    if (s == INVALID_SOCKET) {
        cli_printf(L"    direct connect %s:%d failed (%d)\n", inv.lan, inv.port, WSAGetLastError());
        net_stop();
        return 0;
    }
    char req[128], line[128];
    char* tk = w2u(inv.token);
    _snprintf_s(req, sizeof(req), _TRUNCATE, "JOIN %s", tk ? tk : "");
    free(tk);
    nr_send_line(s, req);
    int r = nr_recv_line(s, line, sizeof(line), 8000);
    int ok = (r > 0 && strncmp(line, "OK", 2) == 0);
    if (!ok) cli_printf(L"    direct handshake failed: r=%d line=%hs\n", r, line);
    if (ok) {
        nr_send_all(s, "PING\n", 5);
        r = nr_recv_line(s, line, sizeof(line), 8000);
        ok = (r > 0 && strncmp(line, "PONG", 4) == 0);
        if (!ok) cli_printf(L"    direct echo failed: r=%d line=%hs\n", r, line);
    }
    closesocket(s);
    net_stop();
    return ok;
}

static int test_guest_engine(int mc_port) {
    SOCKET ls;
    int port = 0;
    if (!nr_make_listener(0, &ls, &port)) {
        cli_print(L"    guest listener failed\n");
        return 0;
    }
    wchar_t token[32];
    nr_token(token, 10);
    test_host_arg* h = (test_host_arg*)xalloc(sizeof(test_host_arg));
    memset(h, 0, sizeof(*h));
    h->ls = ls;
    h->mc_port = mc_port;
    wcsncpy_s(h->token, 32, token, _TRUNCATE);
    nr_spawn(test_manual_host_thread, h, 128 * 1024);

    wchar_t invite[1024];
    nr_invite_direct(L"测试房间", L"127.0.0.1", L"127.0.0.1", port, token, invite, 1024);
    wchar_t err[512] = L"";
    if (!net_join(invite, err, 512)) {
        cli_printf(L"    guest join failed: %s\n", err);
        return 0;
    }
    wchar_t addr[256] = L"";
    int lp = test_wait_address(addr, 256, 12000);
    if (!lp) {
        net_status st;
        net_get_status(&st);
        cli_printf(L"    guest proxy wait failed: %s\n", st.status);
    }
    int ok = (lp > 0 && test_roundtrip(lp));
    if (!ok) cli_printf(L"    guest roundtrip failed: lp=%d\n", lp);
    net_stop();
    return ok;
}

int net_selftest(void) {
    int fail = 0;
    int relay_port = 0;
    if (!relay_start(0, &relay_port)) {
        cli_print(L"[FAIL] 启动测试中转服务器失败\n");
        return 1;
    }
    InterlockedExchange(&g_relay_run, 1);
    HANDLE rh = CreateThread(NULL, 128 * 1024, relay_accept_thread, NULL, 0, NULL);
    if (rh) CloseHandle(rh);

    SOCKET mc = INVALID_SOCKET;
    int mcport = 0;
    if (!nr_make_listener(0, &mc, &mcport)) {
        cli_print(L"[FAIL] 启动模拟 Minecraft 服务器失败\n");
        relay_shutdown();
        return 1;
    }
    g_test_mc_sock = mc;
    HANDLE mh = CreateThread(NULL, 64 * 1024, test_mc_thread, NULL, 0, NULL);
    if (mh) CloseHandle(mh);
    cli_printf(L"[ OK ] 模拟 MC 端口 %d，测试中转端口 %d\n", mcport, relay_port);

    if (test_raw_relay(relay_port, mcport)) cli_print(L"[ OK ] 中转协议握手 / 双向转发\n");
    else { cli_print(L"[FAIL] 中转协议握手 / 双向转发\n"); fail++; }

    if (test_direct_engine(mcport)) cli_print(L"[ OK ] 直连主机 / 邀请码 / 鉴权 / 转发\n");
    else { cli_print(L"[FAIL] 直连主机 / 邀请码 / 鉴权 / 转发\n"); fail++; }
    Sleep(300);

    if (test_guest_engine(mcport)) cli_print(L"[ OK ] 加入方本机代理 / LAN 公告线程 / 转发\n");
    else { cli_print(L"[FAIL] 加入方本机代理 / LAN 公告线程 / 转发\n"); fail++; }
    Sleep(300);

    net_stop();
    InterlockedExchange(&g_test_mc_stop, 1);
    closesocket(mc);
    Sleep(150);
    relay_shutdown();
    cli_printf(L"== nettest: %s ==\n", fail == 0 ? L"全部通过" : L"存在失败");
    return fail == 0 ? 0 : 1;
}

int net_cli_host(int mode, const wchar_t* relay, int mc_port, int seconds) {
    wchar_t err[512] = L"";
    if (!net_create_host(mode, relay, mc_port, 0, NULL, err, 512)) {
        cli_printf(L"ERR=%s\n", err[0] ? err : L"create failed");
        return 1;
    }
    net_status st;
    int ok = 0;
    for (int i = 0; i < 300; i++) {
        net_get_status(&st);
        if (st.invite[0]) { ok = 1; break; }
        if (!st.active) break;
        Sleep(50);
    }
    if (!ok) {
        net_get_status(&st);
        cli_printf(L"ERR=%s\n", st.status[0] ? st.status : L"timeout");
        net_stop();
        return 1;
    }
    cli_printf(L"ROOM=%s\n", st.room);
    cli_printf(L"INVITE=%s\n", st.invite);
    cli_printf(L"STATUS=%s\n", st.status);
    for (int i = 0; i < seconds; i++) {
        Sleep(1000);
        net_get_status(&st);
        if (!st.active) break;
    }
    net_stop();
    return 0;
}

int net_cli_join(const wchar_t* invite, int seconds) {
    wchar_t err[512] = L"";
    if (!net_join(invite, err, 512)) {
        cli_printf(L"ERR=%s\n", err[0] ? err : L"join failed");
        return 1;
    }
    net_status st;
    int ok = 0;
    for (int i = 0; i < 300; i++) {
        net_get_status(&st);
        if (st.address[0]) { ok = 1; break; }
        if (!st.active) break;
        Sleep(50);
    }
    if (!ok) {
        net_get_status(&st);
        cli_printf(L"ERR=%s\n", st.status[0] ? st.status : L"timeout");
        net_stop();
        return 1;
    }
    cli_printf(L"ADDR=%s\n", st.address);
    cli_printf(L"STATUS=%s\n", st.status);
    for (int i = 0; i < seconds; i++) {
        Sleep(1000);
        net_get_status(&st);
        if (!st.active) break;
    }
    net_stop();
    return 0;
}
