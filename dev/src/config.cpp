/* LLcl - config.c : INI persistence (UTF-8, next to the exe, < 1KB) */
#include "llcl.h"

#define CFG_PATH L"\\LLcl.ini"

void cfg_init(cfg* c) { memset(c, 0, sizeof(*c)); }

void cfg_free(cfg* c) {
    free(c->java_path); c->java_path = NULL;
    free(c->player); c->player = NULL;
    free(c->game_dir); c->game_dir = NULL;
    free(c->last_version); c->last_version = NULL;
    free(c->msa_refresh); c->msa_refresh = NULL;
    free(c->msa_access); c->msa_access = NULL;
    free(c->msa_name); c->msa_name = NULL;
    free(c->msa_uuid); c->msa_uuid = NULL;
    free(c->msa_xuid); c->msa_xuid = NULL;
    free(c->msa_client); c->msa_client = NULL;
    free(c->relay_server); c->relay_server = NULL;
    free(c->net_invite); c->net_invite = NULL;
}

static wchar_t* cfg_get(const char* text, const char* key) {
    size_t kl = strlen(key);
    const char* p = text;
    while (p && *p) {
        while (*p == '\r' || *p == '\n') p++;
        const char* eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        const char* line = p;
        /* skip section headers / comments */
        if (linelen > 0 && (line[0] == '[' || line[0] == ';' || line[0] == '#')) { p = eol ? eol + 1 : p + linelen; continue; }
        const char* eq = (const char*)memchr(line, '=', linelen);
        if (eq) {
            size_t nk = (size_t)(eq - line);
            while (nk > 0 && (line[nk - 1] == ' ' || line[nk - 1] == '\t')) nk--;
            if (nk == kl && _strnicmp(line, key, kl) == 0) {
                const char* vs = eq + 1;
                const char* ve = eol ? eol : line + linelen;
                while (vs < ve && (*vs == ' ' || *vs == '\t')) vs++;
                while (ve > vs && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r')) ve--;
                size_t vl = (size_t)(ve - vs);
                char* val = (char*)xalloc(vl + 1);
                memcpy(val, vs, vl);
                val[vl] = 0;
                wchar_t* w = u2w(val);
                free(val);
                return w;
            }
        }
        p = eol ? eol + 1 : p + linelen;
    }
    return NULL;
}

int cfg_load(const wchar_t* path, cfg* c) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 65536) { CloseHandle(h); return 0; }
    char* buf = (char*)xalloc((size_t)sz.QuadPart + 1);
    DWORD rd = 0;
    ReadFile(h, buf, (DWORD)sz.QuadPart, &rd, NULL);
    CloseHandle(h);
    buf[rd] = 0;
    if (rd >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
        memmove(buf, buf + 3, (size_t)rd - 2);

    wchar_t* v;
    if ((v = cfg_get(buf, "java_path"))) { free(c->java_path); c->java_path = v; }
    if ((v = cfg_get(buf, "player_name"))) { free(c->player); c->player = v; }
    if ((v = cfg_get(buf, "game_dir"))) { free(c->game_dir); c->game_dir = v; }
    if ((v = cfg_get(buf, "last_version"))) { free(c->last_version); c->last_version = v; }
    c->mem_opt = 1;   /* 运行内存优化默认开启（ini 可显式关闭） */
    c->dl_threads = 8; /* 下载并发默认 8（ini 可改 16） */
    c->net_mode = 0;
    c->net_port = 0;
    if ((v = cfg_get(buf, "version_isolate"))) {
        c->version_isolate = _wtoi(v) != 0 ? 1 : 0;
        free(v);
    }
    if ((v = cfg_get(buf, "mem_opt"))) {
        c->mem_opt = _wtoi(v) != 0 ? 1 : 0;
        free(v);
    }
    if ((v = cfg_get(buf, "dl_threads"))) {
        int t = _wtoi(v);
        if (t == 8 || t == 16) c->dl_threads = t;
        free(v);
    }
    if ((v = cfg_get(buf, "memory_mb"))) {
        int m = _wtoi(v);
        if (m >= 0 && m <= 65536) c->mem_mb = m;
        free(v);
    }
    if ((v = cfg_get(buf, "msa_refresh"))) { free(c->msa_refresh); c->msa_refresh = v; }
    if ((v = cfg_get(buf, "msa_access"))) { free(c->msa_access); c->msa_access = v; }
    if ((v = cfg_get(buf, "msa_name"))) { free(c->msa_name); c->msa_name = v; }
    if ((v = cfg_get(buf, "msa_uuid"))) { free(c->msa_uuid); c->msa_uuid = v; }
    if ((v = cfg_get(buf, "msa_xuid"))) { free(c->msa_xuid); c->msa_xuid = v; }
    if ((v = cfg_get(buf, "msa_client"))) { free(c->msa_client); c->msa_client = v; }
    if ((v = cfg_get(buf, "relay_server"))) { free(c->relay_server); c->relay_server = v; }
    if ((v = cfg_get(buf, "net_invite"))) { free(c->net_invite); c->net_invite = v; }
    if ((v = cfg_get(buf, "net_mode"))) {
        c->net_mode = _wtoi(v) == 1 ? 1 : 0;
        free(v);
    }
    if ((v = cfg_get(buf, "net_port"))) {
        int p = _wtoi(v);
        if (p >= 0 && p <= 65535) c->net_port = p;
        free(v);
    }
    if (c->game_dir) strip_slash(c->game_dir);
    free(buf);
    return 1;
}

int cfg_save(const wchar_t* path, const cfg* c) {
    wsb b;
    wsb_init(&b);
    wsb_add(&b, L"[LLcl]\r\n");
    wsb_addf(&b, L"java_path=%s\r\n", c->java_path ? c->java_path : L"");
    wsb_addf(&b, L"memory_mb=%d\r\n", c->mem_mb);
    wsb_addf(&b, L"player_name=%s\r\n", c->player ? c->player : L"");
    wsb_addf(&b, L"game_dir=%s\r\n", c->game_dir ? c->game_dir : L"");
    wsb_addf(&b, L"last_version=%s\r\n", c->last_version ? c->last_version : L"");
    wsb_addf(&b, L"version_isolate=%d\r\n", c->version_isolate ? 1 : 0);
    wsb_addf(&b, L"mem_opt=%d\r\n", c->mem_opt ? 1 : 0);
    wsb_addf(&b, L"dl_threads=%d\r\n", (c->dl_threads == 16) ? 16 : 8);
    wsb_addf(&b, L"msa_refresh=%s\r\n", c->msa_refresh ? c->msa_refresh : L"");
    wsb_addf(&b, L"msa_access=%s\r\n", c->msa_access ? c->msa_access : L"");
    wsb_addf(&b, L"msa_name=%s\r\n", c->msa_name ? c->msa_name : L"");
    wsb_addf(&b, L"msa_uuid=%s\r\n", c->msa_uuid ? c->msa_uuid : L"");
    wsb_addf(&b, L"msa_xuid=%s\r\n", c->msa_xuid ? c->msa_xuid : L"");
    wsb_addf(&b, L"msa_client=%s\r\n", c->msa_client ? c->msa_client : L"");
    wsb_addf(&b, L"relay_server=%s\r\n", c->relay_server ? c->relay_server : L"");
    wsb_addf(&b, L"net_mode=%d\r\n", c->net_mode == 1 ? 1 : 0);
    wsb_addf(&b, L"net_port=%d\r\n", c->net_port > 0 ? c->net_port : 0);
    wsb_addf(&b, L"net_invite=%s\r\n", c->net_invite ? c->net_invite : L"");
    char* utf8 = w2u(b.b);
    wsb_free(&b);
    if (!utf8) return 0;
    size_t len = strlen(utf8);
    /* write UTF-8 with BOM so Notepad/ini readers detect it */
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(utf8); return 0; }
    const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
    DWORD wr;
    WriteFile(h, bom, 3, &wr, NULL);
    WriteFile(h, utf8, (DWORD)len, &wr, NULL);
    CloseHandle(h);
    free(utf8);
    return 1;
}
