/* LLcl - auth.c : 微软正版登录（OAuth 设备码 + 手动 token，WinINet，Win7 兼容）
 * 认证链：MSA OAuth → Xbox Live → XSTS → Minecraft Services → profile(UUID/名字)
 */
#include "llcl.h"
#define JSMN_HEADER /* only declarations; implementation lives in jsonx.c */
#include "jsmn.h"
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

#define AUTH_TIMEOUT 20000
#define TOK_CAP 8192          /* access/refresh/XBL/XSTS token 缓冲（JWT 可能很长） */
#define MSA_CLIENT_DEFAULT L"c36a9fb6-4f2a-41ff-90bd-ae7cc92031eb"
#define MSA_DEVICECODE L"https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode"
#define MSA_TOKEN L"https://login.microsoftonline.com/consumers/oauth2/v2.0/token"
#define XBL_AUTH L"https://user.auth.xboxlive.com/user/authenticate"
#define XSTS_AUTH L"https://xsts.auth.xboxlive.com/xsts/authorize"
#define MC_LOGIN L"https://api.minecraftservices.com/authentication/login_with_xbox"
#define MC_PROFILE L"https://api.minecraftservices.com/minecraft/profile"

/* 通用 HTTP 请求（标准 InternetConnect/HttpOpenRequest/HttpSendRequest，Win7 兼容）。
 * method: L"GET"/L"POST"；headers: 宽字符请求头（含结尾 \r\n，可 NULL）；body: UTF-8 字节。
 * 只要传输完成（无论 HTTP 状态码）即返回 1，并把响应体交给调用方（错误体也要读，用于解析 XErr）。
 * 返回 0 表示连接/发送层面失败。 */
static int http_request(const wchar_t* method, const wchar_t* url,
                        const wchar_t* headers, const char* body, size_t bodylen,
                        char** out, size_t* outlen, int* status) {
    *out = NULL; *outlen = 0; if (status) *status = 0;
    HINTERNET inet = InternetOpenW(L"LLcl/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!inet) return 0;
    DWORD tmo = AUTH_TIMEOUT;
    InternetSetOptionW(inet, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
    InternetSetOptionW(inet, INTERNET_OPTION_SEND_TIMEOUT, &tmo, sizeof(tmo));
    InternetSetOptionW(inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo));

    URL_COMPONENTSW uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = L"", urlpath[4096] = L"/";
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = urlpath; uc.dwUrlPathLength = 4095;
    int rc = 0;
    HINTERNET conn = NULL, req = NULL;
    if (!InternetCrackUrlW(url, 0, 0, &uc)) goto done;
    conn = InternetConnectW(inet, host, uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT,
                            NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) goto done;
    DWORD flags = INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD |
                  INTERNET_FLAG_KEEP_CONNECTION | INTERNET_FLAG_NO_UI;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) flags |= INTERNET_FLAG_SECURE;
    req = HttpOpenRequestW(conn, method, urlpath, NULL, NULL, NULL, flags, 0);
    if (!req) goto done;
    DWORD hlen = headers ? (DWORD)wcslen(headers) : 0;
    if (!HttpSendRequestW(req, headers, hlen, (LPVOID)body, body ? (DWORD)bodylen : 0)) goto done;
    DWORD st = 0, stl = sizeof(st);
    if (HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &st, &stl, NULL))
        { if (status) *status = (int)st; }
    size_t cap = 16384, n = 0;
    char* buf = (char*)xalloc(cap + 1);
    char tmp[16384];
    DWORD rd = 0;
    while (InternetReadFile(req, tmp, sizeof(tmp), &rd) && rd > 0) {
        if (n + rd + 1 > cap) { cap = (cap * 3) / 2 + rd; buf = (char*)xrealloc(buf, cap + 1); }
        memcpy(buf + n, tmp, rd);
        n += rd;
        rd = 0;
    }
    buf[n] = 0;
    *out = buf; *outlen = n;
    rc = 1;
done:
    if (req) InternetCloseHandle(req);
    if (conn) InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return rc;
}

static int http_post_json(const wchar_t* url, const char* body, char** out, size_t* outlen, int* status) {
    return http_request(L"POST", url, L"Content-Type: application/json\r\nAccept: application/json\r\n",
                        body, body ? strlen(body) : 0, out, outlen, status);
}

static int http_post_form(const wchar_t* url, const char* body, char** out, size_t* outlen, int* status) {
    return http_request(L"POST", url,
                        L"Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n",
                        body, body ? strlen(body) : 0, out, outlen, status);
}

/* JSON: 取 root 下 key 的字符串值（UTF-8 拷贝） */
static int json_strv(const char* js, const struct jsmntok* root, const char* key,
                     char* out, size_t outsz) {
    const struct jsmntok* t = jm(js, root, key);
    if (!t || t->type != JSMN_STRING) return 0;
    size_t len = (size_t)(t->end - t->start);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, js + t->start, len);
    out[len] = 0;
    return 1;
}

/* JSON: 取 key 对应的数字 primitive（如 XErr 2148916233） */
static int json_intv(const char* js, const struct jsmntok* root, const char* key, long long* out) {
    const struct jsmntok* t = jm(js, root, key);
    if (!t || t->type != JSMN_PRIMITIVE) return 0;
    char tmp[32]; size_t len = (size_t)(t->end - t->start);
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, js + t->start, len); tmp[len] = 0;
    *out = _strtoi64(tmp, NULL, 10);
    return 1;
}

static int json_err(const char* js, const struct jsmntok* root, char* out, size_t outsz) {
    return json_strv(js, root, "error_description", out, outsz)
        || json_strv(js, root, "error", out, outsz)
        || json_strv(js, root, "Message", out, outsz)
        || json_strv(js, root, "message", out, outsz);
}

/* form 参数 URL 编码（只保留字母数字与 -_.~，其余按 UTF-8 字节 %XX 编码） */
void net_urlenc(wsb* b, const wchar_t* s) {
    char* u = w2u(s);
    if (!u) return;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char* p = (const unsigned char*)u; *p; p++) {
        unsigned char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            wchar_t t[2] = { c, 0 };
            wsb_add(b, t);
        } else {
            wchar_t t[8];
            _snwprintf_s(t, 8, _TRUNCATE, L"%%%c%c", hex[c >> 4], hex[c & 15]);
            wsb_add(b, t);
        }
    }
    free(u);
}

/* ---------- 1. 设备码 ---------- */
int msa_devicecode(const wchar_t* client_id, wchar_t* user_code, size_t ucsz,
                   wchar_t* device_code, size_t dsz, wchar_t* verify_url, size_t vsz,
                   wchar_t* err, size_t errsz,
                   int* interval_out, int* expires_out) {
    const wchar_t* cid = (client_id && client_id[0]) ? client_id : MSA_CLIENT_DEFAULT;
    wsb b; wsb_init(&b);
    wsb_add(&b, L"client_id="); wsb_add(&b, cid);
    wsb_add(&b, L"&scope=XboxLive.signin%20offline_access");
    char* body = w2u(b.b);
    wsb_free(&b);
    char* resp = NULL; size_t rl = 0; int st = 0;
    int ok = http_post_form(MSA_DEVICECODE, body, &resp, &rl, &st);
    free(body);
    if (!ok || st != 200) {
        char e[512] = "";
        if (resp) { jsmntok_t* t = NULL; int nt = 0; if (json_parse_mem(resp, rl, (struct jsmntok**)&t, &nt) && nt > 0) json_err(resp, t, e, sizeof(e)); free(t); }
        wchar_t* we = u2w(e[0] ? e : "设备码请求失败（网络不可达）");
        _snwprintf_s(err, errsz, _TRUNCATE, L"%s", we ? we : L"设备码请求失败（网络不可达）");
        free(we); free(resp);
        return 0;
    }
    jsmntok_t* t = NULL; int nt = 0;
    int rc = 0;
    if (json_parse_mem(resp, rl, (struct jsmntok**)&t, &nt) && nt > 0) {
        char v[512];
        char vd[2048]; /* device_code 可达 1000+ 字符，独立大缓冲防截断 */
        if (json_strv(resp, t, "user_code", v, sizeof(v))) { wchar_t* w = u2w(v); if (w) { wcsncpy_s(user_code, ucsz, w, _TRUNCATE); free(w); } }
        if (json_strv(resp, t, "device_code", vd, sizeof(vd))) { wchar_t* w = u2w(vd); if (w) { wcsncpy_s(device_code, dsz, w, _TRUNCATE); free(w); } }
        if (json_strv(resp, t, "verification_uri", v, sizeof(v))) { wchar_t* w = u2w(v); if (w) { wcsncpy_s(verify_url, vsz, w, _TRUNCATE); free(w); } }
        if (interval_out) {
            *interval_out = 5;
            long long x = 0;
            /* interval/expires_in 是 JSON 数字（primitive），不是字符串。 */
            if (json_intv(resp, t, "interval", &x) && x >= 1 && x <= 600)
                *interval_out = (int)x;
        }
        if (expires_out) {
            *expires_out = 900;
            long long x = 0;
            if (json_intv(resp, t, "expires_in", &x) && x >= 60 && x <= 3600)
                *expires_out = (int)x;
        }
        if (device_code[0] == 0 || user_code[0] == 0) {
            _snwprintf_s(err, errsz, _TRUNCATE, L"设备码响应缺少 user_code/device_code");
            free(t); free(resp);
            return 0;
        }
        rc = 1;
    }
    free(t); free(resp);
    return rc;
}

/* ---------- 2. 轮询 token（按 interval 轮询直到成功/失败/过期） ---------- */
int msa_poll_token(const wchar_t* client_id, const wchar_t* device_code,
                   int interval_sec,
                   wchar_t* access, size_t asz, wchar_t* refresh, size_t rsz,
                   wchar_t* err, size_t errsz) {
    const wchar_t* cid = (client_id && client_id[0]) ? client_id : MSA_CLIENT_DEFAULT;
    if (interval_sec < 1) interval_sec = 5;
    int wait_ms = interval_sec * 1000;
    wsb b; wsb_init(&b);
    wsb_add(&b, L"grant_type=urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code&client_id=");
    wsb_add(&b, cid);
    wsb_add(&b, L"&device_code=");
    net_urlenc(&b, device_code);
    char* body = w2u(b.b);
    wsb_free(&b);
    int rc = 0;
    int max_polls = 900 / interval_sec + 5;   /* 按 expires_in 兜底，最多约 15 分钟 */
    for (int i = 0; i < max_polls; i++) {
        char* resp = NULL; size_t rl = 0; int st = 0;
        int ok = http_post_form(MSA_TOKEN, body, &resp, &rl, &st);
        if (ok && resp) {
            jsmntok_t* t = NULL; int nt = 0;
            if (json_parse_mem(resp, rl, (struct jsmntok**)&t, &nt) && nt > 0) {
                char v[TOK_CAP];
                if (json_strv(resp, t, "access_token", v, sizeof(v))) {
                    wchar_t* w = u2w(v); if (w) { wcsncpy_s(access, asz, w, _TRUNCATE); free(w); }
                    if (json_strv(resp, t, "refresh_token", v, sizeof(v))) { wchar_t* w2 = u2w(v); if (w2) { wcsncpy_s(refresh, rsz, w2, _TRUNCATE); free(w2); } }
                    rc = 1;
                } else {
                    /* 注意：状态判断必须用 error 字段（机器可读错误码），
                       不能用 error_description —— 微软的描述文本是
                       "AADSTS70016: ... Authorization is pending. Continue polling"，
                       里面没有下划线形式的 "authorization_pending"，会导致正常的
                       等待授权状态被误判成立即失败。 */
                    char ec[128] = "";
                    char ed[512] = "";
                    json_strv(resp, t, "error", ec, sizeof(ec));
                    json_err(resp, t, ed, sizeof(ed));
                    int pending = strstr(ec, "authorization_pending") != NULL;
                    int slow = strstr(ec, "slow_down") != NULL;
                    if (slow) { Sleep(5000); wait_ms = (interval_sec + 5) * 1000; }   /* slow_down: 间隔再 +5 秒 */
                    if (!pending && !slow) {
                        if (strstr(ec, "expired_token"))
                            _snwprintf_s(err, errsz, _TRUNCATE, L"设备码已过期，请重新登录");
                        else if (strstr(ec, "access_denied"))
                            _snwprintf_s(err, errsz, _TRUNCATE, L"用户拒绝了授权");
                        else {
                            char e[512];
                            if (ed[0]) strncpy_s(e, sizeof(e), ed, _TRUNCATE);
                            else if (ec[0]) strncpy_s(e, sizeof(e), ec, _TRUNCATE);
                            else strncpy_s(e, sizeof(e), "未知错误", _TRUNCATE);
                            wchar_t* we = u2w(e);
                            _snwprintf_s(err, errsz, _TRUNCATE, L"登录失败: %s", we ? we : L"未知错误");
                            free(we);
                        }
                        free(t); free(resp); rc = 0; break;
                    }
                }
                free(t);
            }
        }
        free(resp);
        if (rc) break;
        Sleep(wait_ms);   /* 按服务端 interval 等待；网络失败时也按间隔重试 */
    }
    free(body);
    if (!rc && !err[0]) _snwprintf_s(err, errsz, _TRUNCATE, L"登录超时，请重试");
    return rc;
}

/* Minecraft profile 返回 32 位无横线 UUID；游戏/服务端按带横线格式解析更稳妥。 */
static void dash_uuid(const char* in, char* out, size_t outsz) {
    if (!in || strlen(in) != 32) { strncpy_s(out, outsz, in ? in : "", _TRUNCATE); return; }
    _snprintf_s(out, outsz, _TRUNCATE,
                "%.8s-%.4s-%.4s-%.4s-%.12s",
                in, in + 8, in + 12, in + 16, in + 20);
}

/* ---------- 3. access_token → Minecraft profile ---------- */
static int xbox_chain(const char* access, char* uuid_out, size_t usz, char* name_out, size_t nsz,
                      char* xuid_out, size_t xsz, wchar_t* err, size_t errsz) {
    /* XBL —— AuthMethod 固定为 "RPS"。RpsTicket 前缀：
       现代 MSA OAuth token（JWT "eyJ"，或微软个人账号现在颁发的不透明 "EwA" token）
       一律用 "d="；只有极旧的 Live SDK RPS token（"M.C3"/"M.C5" 开头）才用 "t="。
       注意：EwA 虽不含点号、不是 JWT，但仍必须用 d=，用 t= 会 401。 */
    int dotn = 0;
    for (const char* q = access; *q; q++) if (*q == '.') dotn++;
    const char* prefix = (strncmp(access, "M.C", 3) == 0) ? "t=" : "d=";
    wsb j; wsb_init(&j);
    /* 注意：wsb_addf 内部是宽字符 _vsnwprintf_s，char* 参数必须用 %hs；
       用 %s 会把 UTF-8 字节两两当作宽字符读出，token 直接变乱码导致 XBL 401。 */
    wsb_addf(&j, L"{\"Properties\":{\"AuthMethod\":\"RPS\",\"SiteName\":\"user.auth.xboxlive.com\",\"RpsTicket\":\"%hs%hs\"},\"RelyingParty\":\"http://auth.xboxlive.com\",\"TokenType\":\"JWT\"}", prefix, access);
    char* body = w2u(j.b);
    wsb_free(&j);
    char* resp = NULL; size_t rl = 0; int st = 0;
    if (!http_post_json(XBL_AUTH, body, &resp, &rl, &st) || st != 200) {
        char e[512] = "";
        if (resp) { jsmntok_t* t = NULL; int nt = 0; if (json_parse_mem(resp, rl, (struct jsmntok**)&t, &nt) && nt > 0) json_err(resp, t, e, sizeof(e)); free(t); }
        wchar_t* we = e[0] ? u2w(e) : NULL;
        char head[32] = "";
        strncpy_s(head, sizeof(head), access, _TRUNCATE);
        _snwprintf_s(err, errsz, _TRUNCATE,
                     L"Xbox Live 认证失败（HTTP %d，%s token，前缀 %hs，头 %hs）%s%s",
                     st, dotn >= 2 ? L"JWT" : L"不透明", prefix, head,
                     we ? L": " : L"", we ? we : L"");
        free(we); free(body); free(resp); return 0;
    }
    free(body);
    char xbl_tok[TOK_CAP] = "";
    jsmntok_t* t = NULL; int nt = 0;
    if (!json_parse_mem(resp, rl, (struct jsmntok**)&t, &nt) || nt < 1 ||
        !json_strv(resp, t, "Token", xbl_tok, sizeof(xbl_tok))) {
        _snwprintf_s(err, errsz, _TRUNCATE, L"Xbox Live 响应异常");
        free(t); free(resp); return 0;
    }
    free(t); free(resp);

    /* XSTS —— 失败时通常是 HTTP 401，错误体里带 XErr 数字 */
    wsb j2; wsb_init(&j2);
    wsb_addf(&j2, L"{\"Properties\":{\"SandboxId\":\"RETAIL\",\"UserTokens\":[\"%hs\"]},\"RelyingParty\":\"rp://api.minecraftservices.com/\",\"TokenType\":\"JWT\"}", xbl_tok);
    char* body2 = w2u(j2.b);
    wsb_free(&j2);
    resp = NULL; rl = 0; st = 0;
    int sent = http_post_json(XSTS_AUTH, body2, &resp, &rl, &st);
    free(body2);
    if (!sent || st != 200) {
        long long xerr = 0;
        char e[512] = "";
        if (resp) {
            t = NULL; nt = 0;
            if (json_parse_mem(resp, rl, (struct jsmntok**)&t, &nt) && nt > 0) {
                json_intv(resp, t, "XErr", &xerr);
                json_err(resp, t, e, sizeof(e));
            }
            free(t);
        }
        if (xerr == 2148916233) _snwprintf_s(err, errsz, _TRUNCATE, L"该微软账号没有 Minecraft（Java 版需已购买）");
        else if (xerr == 2148916235) _snwprintf_s(err, errsz, _TRUNCATE, L"Xbox 服务暂时不可用，请稍后再试");
        else if (xerr == 2148916236 || xerr == 2148916237) _snwprintf_s(err, errsz, _TRUNCATE, L"需要成人账号（Xbox 地区/年龄限制）");
        else if (xerr == 2148916238) _snwprintf_s(err, errsz, _TRUNCATE, L"未成年账号需要家长同意（Xbox 家庭组设置）");
        else if (xerr) _snwprintf_s(err, errsz, _TRUNCATE, L"XSTS 认证失败（错误码 %lld）", xerr);
        else if (!sent) _snwprintf_s(err, errsz, _TRUNCATE, L"XSTS 认证失败（网络无法连接）");
        else { wchar_t* we = e[0] ? u2w(e) : NULL;
               _snwprintf_s(err, errsz, _TRUNCATE, L"XSTS 认证失败（HTTP %d）%s%s", st, we ? L": " : L"", we ? we : L""); free(we); }
        free(resp); return 0;
    }
    char xsts_tok[TOK_CAP] = "";
    char uhs[256] = "";
    char xid[64] = "";
    t = NULL; nt = 0;
    if (json_parse_mem(resp, rl, (struct jsmntok**)&t, &nt) && nt > 0) {
        json_strv(resp, t, "Token", xsts_tok, sizeof(xsts_tok));
        const struct jsmntok* dc = jm(resp, t, "DisplayClaims");
        if (dc) {
            const struct jsmntok* xui = jm(resp, dc, "xui");
            if (xui && xui->type == JSMN_ARRAY && xui->size > 0) {
                const struct jsmntok* first = xui + 1;
                json_strv(resp, first, "uhs", uhs, sizeof(uhs));
                json_strv(resp, first, "xid", xid, sizeof(xid));
            }
        }
    }
    free(t); free(resp);
    if (!xsts_tok[0] || !uhs[0]) { _snwprintf_s(err, errsz, _TRUNCATE, L"XSTS 响应异常"); return 0; }

    /* Minecraft login_with_xbox */
    wsb j3; wsb_init(&j3);
    wsb_addf(&j3, L"{\"identityToken\":\"XBL3.0 x=%hs;%hs\"}", uhs, xsts_tok);
    char* body3 = w2u(j3.b);
    wsb_free(&j3);
    resp = NULL; rl = 0; st = 0;
    if (!http_post_json(MC_LOGIN, body3, &resp, &rl, &st) || st != 200) {
        _snwprintf_s(err, errsz, _TRUNCATE, L"Minecraft 认证失败（HTTP %d）", st);
        free(body3); free(resp); return 0;
    }
    free(body3);
    char mc_tok[TOK_CAP] = "";
    t = NULL; nt = 0;
    if (!json_parse_mem(resp, rl, (struct jsmntok**)&t, &nt) || nt < 1 ||
        !json_strv(resp, t, "access_token", mc_tok, sizeof(mc_tok))) {
        _snwprintf_s(err, errsz, _TRUNCATE, L"Minecraft 认证响应异常");
        free(t); free(resp); return 0;
    }
    free(t); free(resp);

    /* profile */
    wchar_t hdr[TOK_CAP + 64];
    _snwprintf_s(hdr, TOK_CAP + 64, _TRUNCATE, L"Authorization: Bearer %S\r\n", mc_tok);
    char* pbuf = NULL; size_t pn = 0; int st2 = 0;
    if (!http_request(L"GET", MC_PROFILE, hdr, NULL, 0, &pbuf, &pn, &st2)) {
        _snwprintf_s(err, errsz, _TRUNCATE, L"获取档案失败（网络无法连接）");
        return 0;
    }
    if (st2 != 200) {
        if (st2 == 404) _snwprintf_s(err, errsz, _TRUNCATE, L"该账号未拥有 Minecraft Java 版（档案不存在）");
        else _snwprintf_s(err, errsz, _TRUNCATE, L"无法获取 Minecraft 档案（HTTP %d）", st2);
        free(pbuf); return 0;
    }
    int rc = 0;
    t = NULL; nt = 0;
    if (json_parse_mem(pbuf, pn, (struct jsmntok**)&t, &nt) && nt > 0) {
        char v[512];
        if (json_strv(pbuf, t, "id", v, sizeof(v))) { dash_uuid(v, uuid_out, usz); rc = 1; }
        if (json_strv(pbuf, t, "name", v, sizeof(v))) { strncpy_s(name_out, nsz, v, _TRUNCATE); }
        else rc = 0;
    }
    if (!rc) _snwprintf_s(err, errsz, _TRUNCATE, L"档案数据异常（可能需要重新登录）");
    free(t); free(pbuf);
    if (rc && xuid_out) {
        if (xid[0]) strncpy_s(xuid_out, xsz, xid, _TRUNCATE);
        else xuid_out[0] = 0;
    }
    return rc;
}

/* ---------- 4. refresh ---------- */
int msa_refresh(const wchar_t* client_id, const wchar_t* refresh,
                wchar_t* access, size_t asz, wchar_t* refresh_new, size_t rsz,
                wchar_t* err, size_t errsz) {
    const wchar_t* cid = (client_id && client_id[0]) ? client_id : MSA_CLIENT_DEFAULT;
    wsb b; wsb_init(&b);
    wsb_add(&b, L"grant_type=refresh_token&client_id=");
    wsb_add(&b, cid);
    wsb_add(&b, L"&refresh_token=");
    net_urlenc(&b, refresh);
    wsb_add(&b, L"&scope=XboxLive.signin%20offline_access");
    char* body = w2u(b.b);
    wsb_free(&b);
    char* resp = NULL; size_t rl = 0; int st = 0;
    int rc = 0;
    if (http_post_form(MSA_TOKEN, body, &resp, &rl, &st) && st == 200 && resp) {
        jsmntok_t* t = NULL; int nt = 0;
        if (json_parse_mem(resp, rl, (struct jsmntok**)&t, &nt) && nt > 0) {
            char v[TOK_CAP];
            if (json_strv(resp, t, "access_token", v, sizeof(v))) {
                wchar_t* w = u2w(v); if (w) { wcsncpy_s(access, asz, w, _TRUNCATE); free(w); }
                if (json_strv(resp, t, "refresh_token", v, sizeof(v))) { wchar_t* w2 = u2w(v); if (w2) { wcsncpy_s(refresh_new, rsz, w2, _TRUNCATE); free(w2); } }
                rc = 1;
            } else {
                char e[512] = ""; json_err(resp, t, e, sizeof(e));
                wchar_t* we = u2w(e[0] ? e : "登录已过期，请重新登录");
                _snwprintf_s(err, errsz, _TRUNCATE, L"刷新失败: %s", we ? we : L"登录已过期，请重新登录"); free(we);
            }
        }
        free(t);
    } else {
        _snwprintf_s(err, errsz, _TRUNCATE, L"刷新失败（网络或登录已过期），请重新登录");
    }
    free(resp); free(body);
    return rc;
}

/* ---------- 5. 手动 token 登录 ---------- */
int msa_token_login(const wchar_t* token, wchar_t* uuid, size_t usz, wchar_t* name, size_t nsz,
                    wchar_t* xuid, size_t xsz, wchar_t* err, size_t errsz) {
    char* tk = w2u(token);
    if (!tk) { _snwprintf_s(err, errsz, _TRUNCATE, L"内部错误"); return 0; }
    char u[128] = "", nm[128] = "", xid[64] = "";
    int rc = xbox_chain(tk, u, sizeof(u), nm, sizeof(nm), xid, sizeof(xid), err, errsz);
    free(tk);
    if (rc) {
        wchar_t* wu = u2w(u); wchar_t* wn = u2w(nm); wchar_t* wx = xid[0] ? u2w(xid) : NULL;
        if (wu) { wcsncpy_s(uuid, usz, wu, _TRUNCATE); free(wu); }
        if (wn) { wcsncpy_s(name, nsz, wn, _TRUNCATE); free(wn); }
        if (xuid) { if (wx) wcsncpy_s(xuid, xsz, wx, _TRUNCATE); else xuid[0] = 0; }
        free(wx);
    }
    return rc;
}

/* ---------- 6. 已存 refresh → 静默恢复登录 ---------- */
int msa_restore(const wchar_t* client_id, const wchar_t* refresh,
                wchar_t* uuid, size_t usz, wchar_t* name, size_t nsz,
                wchar_t* xuid, size_t xsz, wchar_t* access_out, size_t asz,
                wchar_t* refresh_new, size_t rsz, wchar_t* err, size_t errsz) {
    wchar_t access[TOK_CAP] = L"", rn[TOK_CAP] = L"";
    if (!msa_refresh(client_id, refresh, access, TOK_CAP, rn, TOK_CAP, err, errsz)) return 0;
    if (access_out && asz) wcsncpy_s(access_out, asz, access, _TRUNCATE);
    char* tk = w2u(access);
    if (!tk) { _snwprintf_s(err, errsz, _TRUNCATE, L"内部错误"); return 0; }
    char u[128] = "", nm[128] = "", xid[64] = "";
    int rc = xbox_chain(tk, u, sizeof(u), nm, sizeof(nm), xid, sizeof(xid), err, errsz);
    free(tk);
    if (rc) {
        wchar_t* wu = u2w(u); wchar_t* wn = u2w(nm); wchar_t* wx = xid[0] ? u2w(xid) : NULL;
        if (wu) { wcsncpy_s(uuid, usz, wu, _TRUNCATE); free(wu); }
        if (wn) { wcsncpy_s(name, nsz, wn, _TRUNCATE); free(wn); }
        if (xuid) { if (wx) wcsncpy_s(xuid, xsz, wx, _TRUNCATE); else xuid[0] = 0; }
        free(wx);
        if (rn[0]) wcsncpy_s(refresh_new, rsz, rn, _TRUNCATE);
        else if (refresh) wcsncpy_s(refresh_new, rsz, refresh, _TRUNCATE);
    }
    return rc;
}
