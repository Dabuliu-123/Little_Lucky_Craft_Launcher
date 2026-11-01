/* LLcl - jsonx.c : jsmn-based JSON load + token helpers */
#include "jsmn.h"
#include "llcl.h"

#define JSON_MAX_BYTES (8u * 1024u * 1024u)

int json_load_file(const wchar_t* path, char** js_out, struct jsmntok** toks_out, int* ntoks_out) {
    *js_out = NULL; *toks_out = NULL; *ntoks_out = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > JSON_MAX_BYTES) {
        CloseHandle(h); return 0;
    }
    char* js = (char*)xalloc((size_t)sz.QuadPart + 1);
    DWORD rd = 0, total = 0;
    BOOL okr = TRUE;
    while (total < (DWORD)sz.QuadPart) {
        if (!ReadFile(h, js + total, (DWORD)sz.QuadPart - total, &rd, NULL) || rd == 0) { okr = FALSE; break; }
        total += rd;
    }
    CloseHandle(h);
    if (!okr) { free(js); return 0; }
    js[total] = 0;
    /* strip UTF-8 BOM */
    if (total >= 3 && (unsigned char)js[0] == 0xEF && (unsigned char)js[1] == 0xBB && (unsigned char)js[2] == 0xBF) {
        memmove(js, js + 3, (size_t)total - 2);
        js[total - 3] = 0;
    }

    jsmn_parser p;
    jsmn_init(&p);
    int cnt = jsmn_parse(&p, js, strlen(js), NULL, 0);
    if (cnt <= 0) { free(js); return 0; }
    jsmntok_t* toks = (jsmntok_t*)xalloc((size_t)cnt * sizeof(jsmntok_t));
    jsmn_init(&p);
    int cnt2 = jsmn_parse(&p, js, strlen(js), toks, (unsigned)cnt);
    if (cnt2 <= 0) { free(js); free(toks); return 0; }
    *js_out = js;
    *toks_out = (struct jsmntok*)toks;
    *ntoks_out = cnt2;
    return 1;
}

void json_free(char* js, struct jsmntok* toks) {
    free(js);
    free(toks);
}

/* Parse JSON from memory buffer (no BOM strip; length-based). */
int json_parse_mem(const char* js, size_t len, struct jsmntok** toks_out, int* ntoks_out) {
    *toks_out = NULL; *ntoks_out = 0;
    jsmn_parser p;
    jsmn_init(&p);
    int cnt = jsmn_parse(&p, js, len, NULL, 0);
    if (cnt <= 0) return 0;
    jsmntok_t* toks = (jsmntok_t*)xalloc((size_t)cnt * sizeof(jsmntok_t));
    jsmn_init(&p);
    int cnt2 = jsmn_parse(&p, js, len, toks, (unsigned)cnt);
    if (cnt2 <= 0) { free(toks); return 0; }
    *toks_out = (struct jsmntok*)toks;
    *ntoks_out = cnt2;
    return 1;
}

/* Token subtree skip: returns pointer to token AFTER t's entire subtree.
 * Works because jsmn emits tokens in depth-first order; object.size = key-value
 * pairs, array.size = elements. */
const struct jsmntok* tok_skip(const struct jsmntok* t) {
    const jsmntok_t* tok = (const jsmntok_t*)t;
    if (tok->type == JSMN_OBJECT || tok->type == JSMN_ARRAY) {
        const jsmntok_t* p = tok + 1;
        for (int i = 0; i < tok->size; i++) {
            p = tok_skip((const struct jsmntok*)p);
            if (tok->type == JSMN_OBJECT) p = tok_skip((const struct jsmntok*)p);
        }
        return (const struct jsmntok*)p;
    }
    return (const struct jsmntok*)(tok + 1);
}

const struct jsmntok* jm(const char* js, const struct jsmntok* obj, const char* key) {
    const jsmntok_t* o = (const jsmntok_t*)obj;
    if (!o || o->type != JSMN_OBJECT) return NULL;
    size_t klen = strlen(key);
    const struct jsmntok* p = obj + 1;
    for (int i = 0; i < o->size; i++) {
        const jsmntok_t* k = (const jsmntok_t*)p;
        const jsmntok_t* v = k + 1;
        if (k->type == JSMN_STRING && (size_t)(k->end - k->start) == klen &&
            memcmp(js + k->start, key, klen) == 0)
            return (const struct jsmntok*)v;
        p = tok_skip((const struct jsmntok*)k);
        p = tok_skip((const struct jsmntok*)v);
    }
    return NULL;
}

/* Append one Unicode code point to a UTF-8 byte buffer. */
static void utf8_put(char* out, size_t* pos, unsigned int cp) {
    if (cp < 0x80) out[(*pos)++] = (char)cp;
    else if (cp < 0x800) {
        out[(*pos)++] = (char)(0xC0 | (cp >> 6));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[(*pos)++] = (char)(0xE0 | (cp >> 12));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[(*pos)++] = (char)(0xF0 | (cp >> 18));
        out[(*pos)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[(*pos)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static unsigned int hex4(const char* p) {
    unsigned int cp = 0;
    for (int k = 0; k < 4; k++) {
        unsigned char h = (unsigned char)p[k];
        cp <<= 4;
        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
        else return 0xFFFD;
    }
    return cp;
}

/* Raw UTF-8 token span -> unescaped UTF-8 string (malloc).
 * 关键：输出直接是 UTF-8 字节，不能再经 w2u（旧实现把原始 UTF-8 字节
 * 当成 Unicode 码点重新编码，中文/日文等非 ASCII 字符串会变成乱码）。 */
char* jstrdup(const char* js, const struct jsmntok* t) {
    const jsmntok_t* tok = (const jsmntok_t*)t;
    if (!tok || tok->type != JSMN_STRING) return NULL;
    size_t cap = (size_t)(tok->end - tok->start) + 1;
    char* out = (char*)xalloc(cap);
    size_t op = 0;
    int i = tok->start;
    while (i < tok->end) {
        unsigned char c = (unsigned char)js[i];
        if (c == '\\' && i + 1 < tok->end) {
            unsigned char e = (unsigned char)js[i + 1];
            switch (e) {
            case 'n': out[op++] = '\n'; i += 2; continue;
            case 't': out[op++] = '\t'; i += 2; continue;
            case 'r': out[op++] = '\r'; i += 2; continue;
            case 'b': out[op++] = '\b'; i += 2; continue;
            case 'f': out[op++] = '\f'; i += 2; continue;
            case '/': out[op++] = '/'; i += 2; continue;
            case '\\': out[op++] = '\\'; i += 2; continue;
            case '"': out[op++] = '"'; i += 2; continue;
            case 'u': {
                if (i + 6 <= tok->end) {
                    unsigned int cp = hex4(js + i + 2);
                    i += 6;
                    /* surrogate pair: \uD83D\uDE00 */
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= tok->end &&
                        js[i] == '\\' && js[i + 1] == 'u') {
                        unsigned int lo = hex4(js + i + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    utf8_put(out, &op, cp);
                    continue;
                }
                out[op++] = '?'; i += 2; continue;
            }
            default:
                out[op++] = (char)c; i++; continue;
            }
        }
        /* Copy non-escaped bytes verbatim (already UTF-8). */
        out[op++] = (char)c;
        i++;
    }
    out[op] = 0;
    return out;
}

wchar_t* jwstrdup(const char* js, const struct jsmntok* t) {
    char* u = jstrdup(js, t);
    if (!u) return NULL;
    wchar_t* w = u2w(u);
    free(u);
    return w;
}

int jbool(const char* js, const struct jsmntok* t) {
    const jsmntok_t* tok = (const jsmntok_t*)t;
    if (!tok) return 0;
    int len = tok->end - tok->start;
    if (len == 4 && _strnicmp(js + tok->start, "true", 4) == 0) return 1;
    if (len == 5 && _strnicmp(js + tok->start, "false", 5) == 0) return 0;
    return 0;
}
