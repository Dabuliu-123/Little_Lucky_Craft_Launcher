/* LLcl - upnp.cpp : 极简 UPnP IGD 端口映射（直连房间自动开端口）
 *
 * 只实现 WANIPConnection:1 / WANPPPConnection:1 的 AddPortMapping /
 * GetExternalIPAddress / DeletePortMapping，够用即可，不依赖第三方库。
 * 路由器不支持或没开 UPnP 时返回 0，调用方回退到“手动端口映射/中转模式”。
 */
#include "llcl.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

static wchar_t g_control[2048] = L"";
static wchar_t g_service[128] = L"";
static int g_mapped_port = 0;

static const char* stristr2(const char* s, const char* sub) {
    if (!s || !sub) return NULL;
    size_t n = strlen(sub);
    if (!n) return s;
    for (; *s; s++) {
        if (_strnicmp(s, sub, n) == 0) return s;
    }
    return NULL;
}

static int http_read_all(HINTERNET req, char** out, int* out_len) {
    *out = NULL;
    *out_len = 0;
    DWORD cap = 4096, len = 0;
    char* buf = (char*)xalloc(cap);
    for (;;) {
        DWORD got = 0;
        char tmp[4096];
        if (!InternetReadFile(req, tmp, sizeof(tmp), &got)) { free(buf); return 0; }
        if (got == 0) break;
        if (len + got + 1 > cap) {
            while (len + got + 1 > cap) cap *= 2;
            char* nb = (char*)xrealloc(buf, cap);
            if (!nb) { free(buf); return 0; }
            buf = nb;
        }
        memcpy(buf + len, tmp, got);
        len += got;
        if (len > 512 * 1024) break;
    }
    buf[len] = 0;
    *out = buf;
    *out_len = (int)len;
    return 1;
}

static int http_get(const wchar_t* url, char** out, int* out_len) {
    HINTERNET inet = InternetOpenW(L"LLcl/1.3", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!inet) return 0;
    DWORD tv = 1500;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &tv, sizeof(tv));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &tv, sizeof(tv));
    HINTERNET req = InternetOpenUrlW(inet, url, NULL, 0,
                                     INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    int ok = 0;
    if (req) {
        ok = http_read_all(req, out, out_len);
        InternetCloseHandle(req);
    }
    InternetCloseHandle(inet);
    return ok;
}

static int http_post(const wchar_t* url, const char* soap_action, const char* body,
                     char** out, int* out_len) {
    *out = NULL;
    *out_len = 0;
    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!InternetCrackUrlW(url, 0, 0, &uc)) return 0;
    wchar_t host[256], path[1024];
    if (uc.dwHostNameLength >= 255 || uc.dwUrlPathLength >= 1023) return 0;
    wcsncpy_s(host, 256, uc.lpszHostName, uc.dwHostNameLength);
    host[uc.dwHostNameLength] = 0;
    if (uc.dwUrlPathLength) {
        wcsncpy_s(path, 1024, uc.lpszUrlPath, uc.dwUrlPathLength);
        path[uc.dwUrlPathLength] = 0;
    } else {
        wcscpy_s(path, 1024, L"/");
    }
    if (uc.dwExtraInfoLength) {
        size_t l = wcslen(path);
        if (l + uc.dwExtraInfoLength < 1023) {
            wcsncat_s(path, 1024, uc.lpszExtraInfo, uc.dwExtraInfoLength);
        }
    }
    HINTERNET inet = InternetOpenW(L"LLcl/1.3", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!inet) return 0;
    DWORD tv = 1500;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &tv, sizeof(tv));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &tv, sizeof(tv));
    HINTERNET conn = InternetConnectW(inet, host, uc.nPort, NULL, NULL,
                                      INTERNET_SERVICE_HTTP, 0, 0);
    int ok = 0;
    if (conn) {
        DWORD flags = INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD;
        if (uc.nScheme == INTERNET_SCHEME_HTTPS) flags |= INTERNET_FLAG_SECURE;
        HINTERNET req = HttpOpenRequestW(conn, L"POST", path, NULL, NULL, NULL, flags, 0);
        if (req) {
            wchar_t hdr[512];
            wchar_t* act = u2w(soap_action);
            _snwprintf_s(hdr, 512, _TRUNCATE,
                         L"Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s\"\r\n",
                         act ? act : L"");
            free(act);
            if (HttpSendRequestW(req, hdr, (DWORD)-1, (LPVOID)body, (DWORD)strlen(body))) {
                ok = http_read_all(req, out, out_len);
            }
            InternetCloseHandle(req);
        }
        InternetCloseHandle(conn);
    }
    InternetCloseHandle(inet);
    return ok;
}

static int ssdp_location(wchar_t* out, int cap) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    DWORD tv = 1200;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    const char* msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
        "\r\n";
    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    to.sin_addr.s_addr = inet_addr("239.255.255.250");
    sendto(s, msearch, (int)strlen(msearch), 0, (sockaddr*)&to, sizeof(to));

    int found = 0;
    for (int i = 0; i < 4 && !found; i++) {
        char buf[4096];
        sockaddr_in from;
        int fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr*)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        const char* p = stristr2(buf, "LOCATION:");
        if (!p) continue;
        p += 9;
        while (*p == ' ') p++;
        const char* e = p;
        while (*e && *e != '\r' && *e != '\n') e++;
        char url[1024];
        size_t l = (size_t)(e - p);
        if (l >= sizeof(url)) continue;
        memcpy(url, p, l);
        url[l] = 0;
        wchar_t* w = u2w(url);
        if (w) {
            wcsncpy_s(out, cap, w, _TRUNCATE);
            free(w);
            found = 1;
        }
    }
    closesocket(s);
    return found;
}

static int xml_value(const char* xml, const char* tag, char* out, int cap) {
    char open[64], close[64];
    _snprintf_s(open, sizeof(open), _TRUNCATE, "<%s>", tag);
    _snprintf_s(close, sizeof(close), _TRUNCATE, "</%s>", tag);
    const char* p = strstr(xml, open);
    if (!p) return 0;
    p += strlen(open);
    const char* e = strstr(p, close);
    if (!e) return 0;
    size_t n = (size_t)(e - p);
    if (n >= (size_t)cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

static int url_join(const wchar_t* base, const wchar_t* rel, wchar_t* out, int cap) {
    if (wstartswith(rel, L"http://") || wstartswith(rel, L"https://")) {
        wcsncpy_s(out, cap, rel, _TRUNCATE);
        return 1;
    }
    URL_COMPONENTSW uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    if (!InternetCrackUrlW(base, 0, 0, &uc)) return 0;
    wchar_t host[256];
    wcsncpy_s(host, 256, uc.lpszHostName, uc.dwHostNameLength);
    host[uc.dwHostNameLength] = 0;
    if (wstartswith(rel, L"/")) {
        _snwprintf_s(out, cap, _TRUNCATE, L"%s://%s:%u%s",
                     uc.nScheme == INTERNET_SCHEME_HTTPS ? L"https" : L"http",
                     host, uc.nPort, rel);
        return 1;
    }
    wchar_t dir[1024];
    wcsncpy_s(dir, 1024, uc.lpszUrlPath, uc.dwUrlPathLength);
    wchar_t* slash = wcsrchr(dir, L'/');
    if (slash) slash[1] = 0;
    else wcscpy_s(dir, 1024, L"/");
    _snwprintf_s(out, cap, _TRUNCATE, L"%s://%s:%u%s%s",
                 uc.nScheme == INTERNET_SCHEME_HTTPS ? L"https" : L"http",
                 host, uc.nPort, dir, rel);
    return 1;
}

static int upnp_discover(void) {
    wchar_t loc[1024] = L"";
    if (!ssdp_location(loc, 1024)) return 0;
    char *desc = NULL;
    int dl = 0;
    if (!http_get(loc, &desc, &dl) || !desc) return 0;

    const char* svc = strstr(desc, "urn:schemas-upnp-org:service:WANIPConnection:1");
    if (!svc) svc = strstr(desc, "urn:schemas-upnp-org:service:WANPPPConnection:1");
    if (!svc) { free(desc); return 0; }
    char ctrl[1024] = "";
    const char* c = strstr(svc, "<controlURL>");
    if (!c || !xml_value(c, "controlURL", ctrl, sizeof(ctrl))) {
        if (!xml_value(desc, "controlURL", ctrl, sizeof(ctrl))) { free(desc); return 0; }
    }
    wchar_t* rel = u2w(ctrl);
    int ok = rel && url_join(loc, rel, g_control, 2048);
    if (rel) free(rel);
    if (ok) {
        if (strstr(svc, "WANPPPConnection"))
            wcscpy_s(g_service, 128, L"urn:schemas-upnp-org:service:WANPPPConnection:1");
        else
            wcscpy_s(g_service, 128, L"urn:schemas-upnp-org:service:WANIPConnection:1");
    }
    free(desc);
    return ok;
}

static int soap_request(const char* body, const char* action, char** out, int* out_len) {
    char sa[256];
    char* s8 = w2u(g_service);
    _snprintf_s(sa, sizeof(sa), _TRUNCATE, "%s#%s", s8 ? s8 : "", action);
    free(s8);
    return http_post(g_control, sa, body, out, out_len);
}

static int upnp_external_ip(wchar_t* out, int cap) {
    char body[2048];
    char* svc = w2u(g_service);
    _snprintf_s(body, sizeof(body), _TRUNCATE,
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<u:GetExternalIPAddress xmlns:u=\"%s\"></u:GetExternalIPAddress>"
        "</s:Body></s:Envelope>", svc ? svc : "");
    free(svc);
    char* resp = NULL;
    int rl = 0;
    int ok = 0;
    if (soap_request(body, "GetExternalIPAddress", &resp, &rl) && resp) {
        char ip[64] = "";
        if (xml_value(resp, "NewExternalIPAddress", ip, sizeof(ip)) && ip[0]) {
            wchar_t* w = u2w(ip);
            if (w) { wcsncpy_s(out, cap, w, _TRUNCATE); free(w); ok = 1; }
        }
    }
    free(resp);
    return ok;
}

static void local_ip(char* out, int cap) {
    strncpy_s(out, cap, "127.0.0.1", _TRUNCATE);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return;
    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(80);
    to.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(s, (sockaddr*)&to, sizeof(to)) == 0) {
        sockaddr_in me;
        int l = sizeof(me);
        if (getsockname(s, (sockaddr*)&me, &l) == 0)
            inet_ntop(AF_INET, &me.sin_addr, out, cap);
    }
    closesocket(s);
}

int upnp_map_tcp(int port, wchar_t* ext_ip, size_t cap) {
    if (ext_ip && cap) ext_ip[0] = 0;
    if (port <= 0 || port > 65535) return 0;
    g_control[0] = 0;
    if (!upnp_discover()) return 0;
    wchar_t ext[64] = L"";
    if (!upnp_external_ip(ext, 64)) return 0;

    char lan[64];
    local_ip(lan, sizeof(lan));
    char body[4096];
    char* svc = w2u(g_service);
    _snprintf_s(body, sizeof(body), _TRUNCATE,
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<u:AddPortMapping xmlns:u=\"%s\">"
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>%d</NewExternalPort>"
        "<NewProtocol>TCP</NewProtocol>"
        "<NewInternalPort>%d</NewInternalPort>"
        "<NewInternalClient>%s</NewInternalClient>"
        "<NewEnabled>1</NewEnabled>"
        "<NewPortMappingDescription>LLcl</NewPortMappingDescription>"
        "<NewLeaseDuration>7200</NewLeaseDuration>"
        "</u:AddPortMapping></s:Body></s:Envelope>",
        svc ? svc : "", port, port, lan);
    free(svc);
    char* resp = NULL;
    int rl = 0;
    int ok = soap_request(body, "AddPortMapping", &resp, &rl);
    if (ok && resp) {
        if (strstr(resp, "<errorCode>") || strstr(resp, "UPnPError")) ok = 0;
        else if (!strstr(resp, "AddPortMappingResponse")) ok = 0;
    }
    free(resp);
    if (!ok) return 0;
    g_mapped_port = port;
    if (ext_ip) wcsncpy_s(ext_ip, cap, ext, _TRUNCATE);
    return 1;
}

void upnp_unmap_tcp(int port) {
    if (g_mapped_port <= 0 || !g_control[0]) return;
    if (port > 0 && port != g_mapped_port) return;
    char body[2048];
    char* svc = w2u(g_service);
    _snprintf_s(body, sizeof(body), _TRUNCATE,
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
        "<u:DeletePortMapping xmlns:u=\"%s\">"
        "<NewRemoteHost></NewRemoteHost>"
        "<NewExternalPort>%d</NewExternalPort>"
        "<NewProtocol>TCP</NewProtocol>"
        "</u:DeletePortMapping></s:Body></s:Envelope>", svc ? svc : "", g_mapped_port);
    free(svc);
    char* resp = NULL;
    int rl = 0;
    soap_request(body, "DeletePortMapping", &resp, &rl);
    free(resp);
    g_mapped_port = 0;
}
