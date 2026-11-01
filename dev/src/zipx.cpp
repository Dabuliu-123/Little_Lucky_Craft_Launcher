/* LLcl - zipx.c : 极简 ZIP 解压器（自研，零依赖，Win7 安全）
 * EOCD → Central Directory → Local Header；支持 store(0)/deflate(8)；
 * CRC32 校验；UTF-8 文件名；拒绝路径穿越/绝对路径。
 */
#include "llcl.h"
#include <string.h>
#include <stdlib.h>

/* ---------------- crc32 ---------------- */
static uint32_t crc_tab[256];
static int crc_ready = 0;
static void crc_init(void) {
    if (crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_tab[i] = c;
    }
    crc_ready = 1;
}
static uint32_t crc32_bytes(uint32_t crc, const unsigned char* p, size_t n) {
    for (size_t i = 0; i < n; i++) crc = crc_tab[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

/* ---------------- bit reader (LSB first, like DEFLATE) ---------------- */
typedef struct { const unsigned char* in; size_t len; size_t pos; uint32_t bb; int bc; } btr;
static int bbits(btr* b, int n) {
    while (b->bc < n) {
        if (b->pos >= b->len) return 0;
        b->bb |= (uint32_t)b->in[b->pos++] << b->bc;
        b->bc += 8;
    }
    uint32_t v = b->bb & ((1u << n) - 1);
    b->bb >>= n;
    b->bc -= n;
    return (int)v;
}

/* ---------------- canonical Huffman ---------------- */
typedef struct { short count[16]; short symbol[288]; } huff;
static int huff_build(huff* h, const unsigned char* l, int n) {
    int len;
    memset(h, 0, sizeof(*h));
    for (len = 0; len < n; len++) h->count[l[len]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;      /* over-subscribed */
    }
    int offs[16];
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (len = 0; len < n; len++)
        if (l[len] != 0) h->symbol[offs[l[len]]++] = (short)len;
    return 0;
}
static int huff_decode(btr* b, const huff* h) {
    int len = 1, code = 0, first = 0, index = 0;
    for (;;) {
        code |= bbits(b, 1);
        int cnt = h->count[len];
        if (code - cnt < first) return h->symbol[index + (code - first)];
        index += cnt;
        first += cnt;
        first <<= 1;
        code <<= 1;
        len++;
        if (len > 15) return -1;
    }
}

/* ---------------- raw deflate ---------------- */
static const short len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short len_ext[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short dist_ext[30]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static const short clen_order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

static huff g_fixed_lit, g_fixed_dist;
static int g_fixed_ready = 0;
static void fixed_tables(void) {
    if (g_fixed_ready) return;
    unsigned char l[288];
    int i;
    for (i = 0; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    huff_build(&g_fixed_lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    huff_build(&g_fixed_dist, l, 30);
    g_fixed_ready = 1;
}

/* inflate raw DEFLATE stream; returns output byte count or -1 */
static long inflate_raw(const unsigned char* in, size_t in_len, unsigned char* out, size_t out_len) {
    btr b;
    b.in = in; b.len = in_len; b.pos = 0; b.bb = 0; b.bc = 0;
    size_t op = 0;
    fixed_tables();
    for (;;) {
        int last = bbits(&b, 1);
        int type = bbits(&b, 2);
        if (type == 0) {
            /* stored: discard partial bits to byte boundary */
            b.bc = 0;
            b.bb = 0;
            size_t len = (size_t)bbits(&b, 16);
            bbits(&b, 16); /* nlen ignored */
            if (b.pos + len > b.len) return -1;
            if (op + len > out_len) return -1;
            memcpy(out + op, b.in + b.pos, len);
            op += len;
            b.pos += len;
        } else if (type == 1 || type == 2) {
            const huff* lit;
            const huff* dist;
            if (type == 1) {
                lit = &g_fixed_lit;
                dist = &g_fixed_dist;
            } else {
                int hlit = bbits(&b, 5) + 257;
                int hdist = bbits(&b, 5) + 1;
                int hclen = bbits(&b, 4) + 4;
                unsigned char cl[19];
                memset(cl, 0, 19);
                for (int i = 0; i < hclen; i++) cl[clen_order[i]] = (unsigned char)bbits(&b, 3);
                huff clh;
                if (huff_build(&clh, cl, 19)) return -1;
                unsigned char lens[320];
                memset(lens, 0, sizeof(lens));
                int i = 0;
                while (i < hlit + hdist) {
                    int sym = huff_decode(&b, &clh);
                    if (sym < 0 || sym > 18) return -1;
                    if (sym < 16) lens[i++] = (unsigned char)sym;
                    else if (sym == 16) {
                        if (i == 0) return -1;
                        int rep = bbits(&b, 2) + 3;
                        unsigned char prev = lens[i - 1];
                        while (rep-- > 0 && i < hlit + hdist) lens[i++] = prev;
                    } else if (sym == 17) {
                        int rep = bbits(&b, 3) + 3;
                        i += rep;
                    } else {
                        int rep = bbits(&b, 7) + 11;
                        i += rep;
                    }
                }
                if (i != hlit + hdist) return -1;
                huff dl, dd;
                if (huff_build(&dl, lens, hlit)) return -1;
                if (huff_build(&dd, lens + hlit, hdist)) return -1;
                lit = &dl;
                dist = &dd;
            }
            for (;;) {
                int sym = huff_decode(&b, lit);
                if (sym < 0) return -1;
                if (sym < 256) {
                    if (op >= out_len) return -1;
                    out[op++] = (unsigned char)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return -1;
                    int llen = len_base[sym] + bbits(&b, len_ext[sym]);
                    int dsym = huff_decode(&b, dist);
                    if (dsym < 0 || dsym >= 30) return -1;
                    int ddist = dist_base[dsym] + bbits(&b, dist_ext[dsym]);
                    if ((size_t)ddist > op) return -1;
                    if (op + (size_t)llen > out_len) return -1;
                    for (int k = 0; k < llen; k++) out[op] = out[op - ddist], op++;
                }
            }
        } else {
            return -1; /* type 3 invalid */
        }
        if (last) break;
    }
    return (long)op;
}

/* 给 server.cpp 的 gzip 解压用：解 raw DEFLATE 到调用方给的容量 */
unsigned char* zip_inflate_raw(const unsigned char* in, size_t in_len, size_t out_cap, size_t* out_len) {
    if (!in || !out_cap || !out_len) return NULL;
    unsigned char* out = (unsigned char*)xalloc(out_cap);
    long n = inflate_raw(in, in_len, out, out_cap);
    if (n < 0) { free(out); return NULL; }
    *out_len = (size_t)n;
    return out;
}

/* ---------------- zip parsing ---------------- */
typedef struct {
    uint32_t crc, csize, usize, lho;
    unsigned char name[512];
    unsigned short method;
} zent;
typedef struct { zent* e; int n, cap; } zlist;

static uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int zip_parse(const unsigned char* buf, size_t len, zlist* zl) {
    /* find EOCD within last 64K+22 */
    size_t start = len > 65557 ? len - 65557 : 0;
    size_t eocd = 0;
    int found = 0;
    if (len >= 22) {
        for (size_t i = len - 22 + 1; i-- > start;) {
            if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) { eocd = i; found = 1; break; }
        }
    }
    if (!found) return 0;
    uint32_t cd_count = rd16(buf + eocd + 10);
    uint32_t cd_size = rd32(buf + eocd + 12);
    uint32_t cd_off = rd32(buf + eocd + 16);
    if ((size_t)cd_off + cd_size > len) return 0;
    size_t p = cd_off;
    for (uint32_t i = 0; i < cd_count; i++) {
        if (p + 46 > len) return 0;
        if (rd32(buf + p) != 0x02014b50u) return 0;
        unsigned short namelen = rd16(buf + p + 28);
        unsigned short extralen = rd16(buf + p + 30);
        unsigned short commentlen = rd16(buf + p + 32);
        if (p + 46 + (size_t)namelen + extralen + commentlen > len) return 0;
        zent e;
        e.method = rd16(buf + p + 10);
        e.crc = rd32(buf + p + 16);
        e.csize = rd32(buf + p + 20);
        e.usize = rd32(buf + p + 24);
        e.lho = rd32(buf + p + 42);
        unsigned short nl = namelen;
        if (nl >= sizeof(e.name)) nl = (unsigned short)(sizeof(e.name) - 1);
        memcpy(e.name, buf + p + 46, nl);
        e.name[nl] = 0;
        p += 46 + (size_t)namelen + extralen + commentlen;
        /* directory entry or unsafe path -> skip */
        size_t namel = strlen((const char*)e.name);
        if (namel && e.name[namel - 1] == '/') continue;
        int unsafe = 0;
        if (e.name[0] == '/' || strchr((const char*)e.name, '\\') || strstr((const char*)e.name, "..")) unsafe = 1;
        if (namel > 1 && e.name[1] == ':') unsafe = 1;
        if (unsafe) continue;
        if (zl->n >= zl->cap) {
            zl->cap = zl->cap ? zl->cap * 2 : 64;
            zl->e = (zent*)xrealloc(zl->e, (size_t)zl->cap * sizeof(zent));
        }
        zl->e[zl->n++] = e;
    }
    return 1;
}

/* ---------------- public API ---------------- */
/* 解压 zip 中文件名以 prefix 开头（NULL=全部）的文件到 dest_dir。
 * 返回 1 表示 zip 读取成功（out_count=实际解出数）；失败返回 0。 */
int zip_extract_prefix(const wchar_t* zip_path, const char* prefix, const wchar_t* dest_dir, int* out_count) {
    *out_count = 0;
    if (!file_exists(zip_path) || !dir_exists(dest_dir)) return 0;
    size_t flen = 0;
    FILE* f = _wfopen(zip_path, L"rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 22 || sz > 256L * 1024 * 1024) { fclose(f); return 0; }
    flen = (size_t)sz;
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)xalloc(flen);
    if (fread(buf, 1, flen, f) != flen) { fclose(f); free(buf); return 0; }
    fclose(f);
    zlist zl;
    memset(&zl, 0, sizeof(zl));
    int ok = zip_parse(buf, flen, &zl);
    if (!ok) { free(buf); free(zl.e); return 0; }
    crc_init();
    int n = 0;
    size_t plen = prefix ? strlen(prefix) : 0;
    for (int i = 0; i < zl.n; i++) {
        zent* e = &zl.e[i];
        if (plen && strncmp((const char*)e->name, prefix, plen) != 0) continue;
        if (e->method != 0 && e->method != 8) continue;
        if (e->usize > 256u * 1024 * 1024) continue;
        if ((size_t)e->lho + 30 > flen) continue;
        if (rd32(buf + e->lho) != 0x04034b50u) continue;
        size_t dstart = (size_t)e->lho + 30 + rd16(buf + e->lho + 26) + rd16(buf + e->lho + 28);
        if (dstart + e->csize > flen) continue;
        const char* rel = (const char*)e->name + plen;
        if (*rel == '/') rel++;
        if (!*rel) continue;
        /* normalize separators and drop any residual ".." segments */
        char reln[512];
        size_t w = 0;
        const char* s = rel;
        while (*s && w < 510) {
            if (*s == '/' || *s == '\\') {
                reln[w++] = '\\';
                s++;
                while (*s == '/' || *s == '\\') s++;
            } else {
                reln[w++] = *s++;
            }
        }
        reln[w] = 0;
        if (strstr(reln, "..")) continue;
        wchar_t* relw = u2w(reln);
        wchar_t* outpath = path_join(dest_dir, relw);
        free(relw);
        wchar_t* slash = wcsrchr(outpath, L'\\');
        if (slash) {
            *slash = 0;
            ensure_dir_rec(outpath);
            *slash = L'\\';
        }
        unsigned char* out = (unsigned char*)xalloc((size_t)e->usize ? e->usize : 1);
        long got = -1;
        if (e->method == 0) {
            memcpy(out, buf + dstart, e->usize);
            got = (long)e->usize;
        } else {
            got = inflate_raw(buf + dstart, e->csize, out, e->usize);
        }
        int written = 0;
        if (got == (long)e->usize) {
            uint32_t crc = crc32_bytes(0xFFFFFFFFu, out, (size_t)e->usize) ^ 0xFFFFFFFFu;
            if (crc == e->crc) {
                FILE* o = _wfopen(outpath, L"wb");
                if (o) {
                    written = fwrite(out, 1, (size_t)e->usize, o) == e->usize;
                    fclose(o);
                }
            }
        }
        if (written) n++;
        free(out);
        free(outpath);
    }
    free(buf);
    free(zl.e);
    *out_count = n;
    return 1;
}

/* 平铺解压 zip 内所有 .dll 到 dest_dir 根目录（忽略 jar 内路径，只取文件名）。
 * 新版 MC（26.x）的 java.library.path 指向 natives/java 等子目录，且 natives jar
 * 内的 dll 带 windows/x64/org/lwjgl/ 平台前缀，必须剥前缀平铺才能被 LoadLibrary 找到。 */
int zip_extract_dlls(const wchar_t* zip_path, const wchar_t* dest_dir, int* out_count) {
    *out_count = 0;
    if (!file_exists(zip_path) || !dir_exists(dest_dir)) return 0;
    size_t flen = 0;
    FILE* f = _wfopen(zip_path, L"rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 22 || sz > 256L * 1024 * 1024) { fclose(f); return 0; }
    flen = (size_t)sz;
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)xalloc(flen);
    if (fread(buf, 1, flen, f) != flen) { fclose(f); free(buf); return 0; }
    fclose(f);
    zlist zl;
    memset(&zl, 0, sizeof(zl));
    int ok = zip_parse(buf, flen, &zl);
    if (!ok) { free(buf); free(zl.e); return 0; }
    crc_init();
    int n = 0;
    for (int i = 0; i < zl.n; i++) {
        zent* e = &zl.e[i];
        if (e->method != 0 && e->method != 8) continue;
        if (e->usize > 256u * 1024 * 1024) continue;
        if ((size_t)e->lho + 30 > flen) continue;
        if (rd32(buf + e->lho) != 0x04034b50u) continue;
        size_t dstart = (size_t)e->lho + 30 + rd16(buf + e->lho + 26) + rd16(buf + e->lho + 28);
        if (dstart + e->csize > flen) continue;
        /* 只取文件名（basename），跳过目录项 */
        const char* base = (const char*)e->name;
        const char* p = base;
        while (*p) { if (*p == '/' || *p == '\\') base = p + 1; p++; }
        if (!*base) continue;
        /* 只平铺 dll */
        size_t bl = strlen(base);
        if (bl < 5 || (_stricmp(base + bl - 4, ".dll") != 0)) continue;
        wchar_t* relw = u2w(base);
        wchar_t* outpath = path_join(dest_dir, relw);
        free(relw);
        /* 覆盖写：调用方在指纹变化时会先清空 natives；
           若上次解压残留同名旧 dll，也要用本次的覆盖掉。 */
        unsigned char* out = (unsigned char*)xalloc((size_t)e->usize ? e->usize : 1);
        long got = -1;
        if (e->method == 0) {
            memcpy(out, buf + dstart, e->usize);
            got = (long)e->usize;
        } else {
            got = inflate_raw(buf + dstart, e->csize, out, e->usize);
        }
        int written = 0;
        if (got == (long)e->usize) {
            uint32_t crc = crc32_bytes(0xFFFFFFFFu, out, (size_t)e->usize) ^ 0xFFFFFFFFu;
            if (crc == e->crc) {
                FILE* o = _wfopen(outpath, L"wb");
                if (o) {
                    written = fwrite(out, 1, (size_t)e->usize, o) == e->usize;
                    fclose(o);
                }
            }
        }
        if (written) n++;
        free(out);
        free(outpath);
    }
    free(buf);
    free(zl.e);
    *out_count = n;
    return 1;
}

/* 读取 zip 中指定文件（精确文件名，如 "install_profile.json"）到内存。
 * 返回 malloc 缓冲（以 0 结尾，便于按文本解析），*out_len=文件实际大小；失败返回 NULL。 */
unsigned char* zip_read_file(const wchar_t* zip_path, const char* target, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (!file_exists(zip_path)) return NULL;
    FILE* f = _wfopen(zip_path, L"rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 22 || sz > 256L * 1024 * 1024) { fclose(f); return NULL; }
    size_t flen = (size_t)sz;
    fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)xalloc(flen);
    if (fread(buf, 1, flen, f) != flen) { fclose(f); free(buf); return NULL; }
    fclose(f);
    zlist zl;
    memset(&zl, 0, sizeof(zl));
    int ok = zip_parse(buf, flen, &zl);
    if (!ok) { free(buf); free(zl.e); return NULL; }
    crc_init();
    unsigned char* result = NULL;
    for (int i = 0; i < zl.n && !result; i++) {
        zent* e = &zl.e[i];
        if (strcmp((const char*)e->name, target) != 0) continue;
        if (e->method != 0 && e->method != 8) continue;
        if ((size_t)e->lho + 30 > flen) continue;
        if (rd32(buf + e->lho) != 0x04034b50u) continue;
        size_t dstart = (size_t)e->lho + 30 + rd16(buf + e->lho + 26) + rd16(buf + e->lho + 28);
        if (dstart + e->csize > flen) continue;
        unsigned char* out = (unsigned char*)xalloc((size_t)e->usize + 1);
        long got = -1;
        if (e->method == 0) { memcpy(out, buf + dstart, e->usize); got = (long)e->usize; }
        else got = inflate_raw(buf + dstart, e->csize, out, e->usize);
        if (got == (long)e->usize) {
            uint32_t crc = crc32_bytes(0xFFFFFFFFu, out, (size_t)e->usize) ^ 0xFFFFFFFFu;
            if (crc == e->crc) {
                out[e->usize] = 0;
                result = out;
                if (out_len) *out_len = (size_t)e->usize;
            }
        }
        if (!result) free(out);
    }
    free(buf);
    free(zl.e);
    return result;
}
