/* LLcl - server.c : 服务器列表管理 (servers.dat NBT 读写)
 * 仅支持 Minecraft 1.3+ 使用的服务器列表格式（Gzip 压缩的 NBT）。
 * 结构：根 Compound -> "servers" List[Compound] -> 每个服务器包含 name/ip/icon/acceptTextures 等。
 * 为保持极简，只解析/写入必要字段：name, ip, hidden, icon(可选)。
 */
#include "llcl.h"
// Minimal zlib definitions for dynamic loading
#define ZLIB_VERSION "1.2.13"
#define ZLIB_VERNUM 0x12d0

#define Z_NO_FLUSH      0
#define Z_PARTIAL_FLUSH 1
#define Z_SYNC_FLUSH    2
#define Z_FULL_FLUSH    3
#define Z_FINISH        4
#define Z_BLOCK         5
#define Z_TREES         6

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_NEED_DICT     2
#define Z_ERRNO        (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)
#define Z_VERSION_ERROR (-6)

#define Z_NO_COMPRESSION         0
#define Z_BEST_SPEED             1
#define Z_BEST_COMPRESSION       9
#define Z_DEFAULT_COMPRESSION  (-1)
#define Z_FILTERED            1
#define Z_HUFFMAN_ONLY        2
#define Z_RLE                 3
#define Z_FIXED               4
#define Z_DEFAULT_STRATEGY    0

#define Z_BINARY   0
#define Z_TEXT     1
#define Z_ASCII    Z_TEXT
#define Z_UNKNOWN  2

#define Z_DEFLATED   8

#define Z_NULL  0

typedef unsigned char  Byte;
typedef unsigned char  Bytef;
typedef unsigned int   uInt;
typedef unsigned long  uLong;
typedef void*          voidpf;
typedef void*          voidp;
typedef long           z_off_t;

typedef struct z_stream_s {
    const Bytef *next_in;
    uInt     avail_in;
    uLong    total_in;
    Bytef    *next_out;
    uInt     avail_out;
    uLong    total_out;
    const char *msg;
    voidpf   state;
    voidpf   zalloc;
    voidpf   zfree;
    voidpf   opaque;
    int      data_type;
    uLong    adler;
    uLong    reserved;
} z_stream;

typedef z_stream *z_streamp;

#define Z_OK            0
#define Z_STREAM_END    1
#define Z_NEED_DICT     2
#define Z_ERRNO        (-1)
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)
#define Z_VERSION_ERROR (-6)

#define Z_NO_COMPRESSION         0
#define Z_BEST_SPEED             1
#define Z_BEST_COMPRESSION       9
#define Z_DEFAULT_COMPRESSION  (-1)
#define Z_FILTERED            1
#define Z_HUFFMAN_ONLY        2
#define Z_RLE                 3
#define Z_FIXED               4
#define Z_DEFAULT_STRATEGY    0

#define Z_BINARY   0
#define Z_TEXT     1
#define Z_ASCII    Z_TEXT
#define Z_UNKNOWN  2

#define Z_DEFLATED   8

#define Z_NULL  0

typedef unsigned char  Byte;
typedef unsigned char  Bytef;
typedef unsigned int   uInt;
typedef unsigned long  uLong;
typedef void*          voidpf;
typedef void*          voidp;
typedef long           z_off_t;

/* ---------------- Gzip 解压/压缩 ----------------
 * 不再依赖 zlib1.dll（Win7 机器通常没有）。解压复用 zipx.cpp 的 raw
 * DEFLATE；压缩使用合法的 gzip “stored” 块：servers.dat 通常只有几百字节，
 * 不压缩也不会变大多少，换来零第三方 DLL 依赖。 */
static uint32_t srv_crc32(const unsigned char* p, size_t n) {
    static uint32_t tab[256];
    static int ready = 0;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static int gzip_decompress(const unsigned char* in, size_t in_len, unsigned char** out, size_t* out_len) {
    if (!in || in_len < 18) return 0;
    if (in[0] != 0x1F || in[1] != 0x8B || in[2] != 8) return 0;
    size_t p = 10;
    unsigned char flg = in[3];
    if (flg & 4) { /* FEXTRA */
        if (p + 2 > in_len) return 0;
        size_t xlen = (size_t)in[p] | ((size_t)in[p + 1] << 8);
        p += 2 + xlen;
    }
    if (flg & 8) { while (p < in_len && in[p]) p++; p++; }   /* FNAME */
    if (flg & 16) { while (p < in_len && in[p]) p++; p++; }  /* FCOMMENT */
    if (flg & 2) p += 2;                                     /* FHCRC */
    if (p + 8 > in_len) return 0;
    uint32_t isize = (uint32_t)in[in_len - 4] | ((uint32_t)in[in_len - 3] << 8) |
                     ((uint32_t)in[in_len - 2] << 16) | ((uint32_t)in[in_len - 1] << 24);
    if (isize == 0 || isize > 8u * 1024 * 1024) return 0;
    size_t got = 0;
    unsigned char* buf = zip_inflate_raw(in + p, in_len - p - 8, (size_t)isize, &got);
    if (!buf) return 0;
    if (got != isize) { free(buf); return 0; }
    /* CRC 校验（Minecraft 写出的 gzip 带 CRC） */
    uint32_t crc = (uint32_t)in[in_len - 8] | ((uint32_t)in[in_len - 7] << 8) |
                   ((uint32_t)in[in_len - 6] << 16) | ((uint32_t)in[in_len - 5] << 24);
    if (srv_crc32(buf, got) != crc) { free(buf); return 0; }
    *out = buf;
    *out_len = got;
    return 1;
}

static int gzip_compress(const unsigned char* in, size_t in_len, unsigned char** out, size_t* out_len) {
    size_t blocks = in_len ? ((in_len + 65534) / 65535) : 1;
    size_t cap = 10 + in_len + blocks * 5 + 8;
    unsigned char* o = (unsigned char*)xalloc(cap);
    size_t p = 0;
    o[p++] = 0x1F; o[p++] = 0x8B; o[p++] = 8; o[p++] = 0;
    o[p++] = 0; o[p++] = 0; o[p++] = 0; o[p++] = 0;
    o[p++] = 0; o[p++] = 3;   /* OS = Unix */
    size_t off = 0;
    do {
        size_t n = in_len - off;
        if (n > 65535) n = 65535;
        int final = (off + n >= in_len);
        o[p++] = (unsigned char)(final ? 1 : 0);
        o[p++] = (unsigned char)(n & 0xFF);
        o[p++] = (unsigned char)((n >> 8) & 0xFF);
        o[p++] = (unsigned char)(~n & 0xFF);
        o[p++] = (unsigned char)((~n >> 8) & 0xFF);
        if (n) memcpy(o + p, in + off, n);
        p += n;
        off += n;
    } while (off < in_len);
    uint32_t crc = srv_crc32(in, in_len);
    o[p++] = (unsigned char)(crc & 0xFF);
    o[p++] = (unsigned char)((crc >> 8) & 0xFF);
    o[p++] = (unsigned char)((crc >> 16) & 0xFF);
    o[p++] = (unsigned char)((crc >> 24) & 0xFF);
    o[p++] = (unsigned char)(in_len & 0xFF);
    o[p++] = (unsigned char)((in_len >> 8) & 0xFF);
    o[p++] = (unsigned char)((in_len >> 16) & 0xFF);
    o[p++] = (unsigned char)((in_len >> 24) & 0xFF);
    *out = o;
    *out_len = p;
    return 1;
}

/* ---------------- NBT tag types ---------------- */
#define TAG_END        0
#define TAG_BYTE       1
#define TAG_SHORT      2
#define TAG_INT        3
#define TAG_LONG       4
#define TAG_FLOAT      5
#define TAG_DOUBLE     6
#define TAG_BYTE_ARRAY 7
#define TAG_STRING     8
#define TAG_LIST       9
#define TAG_COMPOUND  10
#define TAG_INT_ARRAY 11
#define TAG_LONG_ARRAY 12

/* ---------------- NBT reader ---------------- */
typedef struct {
    const unsigned char* data;
    size_t size;
    size_t pos;
} nbt_reader;

static int nbt_read_byte(nbt_reader* r, unsigned char* out) {
    if (r->pos >= r->size) return 0;
    *out = r->data[r->pos++];
    return 1;
}

static int nbt_read_short(nbt_reader* r, short* out) {
    unsigned char b1, b2;
    if (!nbt_read_byte(r, &b1) || !nbt_read_byte(r, &b2)) return 0;
    *out = (short)((b1 << 8) | b2);
    return 1;
}

static int nbt_read_int(nbt_reader* r, int* out) {
    unsigned char b1, b2, b3, b4;
    if (!nbt_read_byte(r, &b1) || !nbt_read_byte(r, &b2) || !nbt_read_byte(r, &b3) || !nbt_read_byte(r, &b4)) return 0;
    *out = (int)((b1 << 24) | (b2 << 16) | (b3 << 8) | b4);
    return 1;
}

static int nbt_read_long(nbt_reader* r, long long* out) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) if (!nbt_read_byte(r, &b[i])) return 0;
    *out = ((long long)b[0] << 56) | ((long long)b[1] << 48) | ((long long)b[2] << 40) | ((long long)b[3] << 32) |
           ((long long)b[4] << 24) | ((long long)b[5] << 16) | ((long long)b[6] << 8) | (long long)b[7];
    return 1;
}

static int nbt_read_string(nbt_reader* r, char** out) {
    short len;
    if (!nbt_read_short(r, &len)) return 0;
    if (len < 0 || (size_t)len > r->size - r->pos) return 0;
    *out = (char*)xalloc((size_t)len + 1);
    memcpy(*out, r->data + r->pos, (size_t)len);
    (*out)[len] = 0;
    r->pos += (size_t)len;
    return 1;
}

static void nbt_skip_tag(nbt_reader* r, unsigned char type);

/* 递归跳过 tag（包括 payload） */
static void nbt_skip_payload(nbt_reader* r, unsigned char type) {
    switch (type) {
    case TAG_BYTE: r->pos += 1; break;
    case TAG_SHORT: r->pos += 2; break;
    case TAG_INT: r->pos += 4; break;
    case TAG_LONG: r->pos += 8; break;
    case TAG_FLOAT: r->pos += 4; break;
    case TAG_DOUBLE: r->pos += 8; break;
    case TAG_BYTE_ARRAY: {
        int len; nbt_read_int(r, &len);
        if (len >= 0) r->pos += (size_t)len;
        break;
    }
    case TAG_STRING: {
        short len; nbt_read_short(r, &len);
        if (len >= 0) r->pos += (size_t)len;
        break;
    }
    case TAG_LIST: {
        unsigned char child_type; int count;
        nbt_read_byte(r, &child_type); nbt_read_int(r, &count);
        for (int i = 0; i < count; i++) nbt_skip_payload(r, child_type);
        break;
    }
    case TAG_COMPOUND: {
        while (1) {
            unsigned char t; nbt_read_byte(r, &t);
            if (t == TAG_END) break;
            char* name = NULL; nbt_read_string(r, &name); free(name);
            nbt_skip_payload(r, t);
        }
        break;
    }
    case TAG_INT_ARRAY: {
        int len; nbt_read_int(r, &len);
        if (len >= 0) r->pos += (size_t)len * 4;
        break;
    }
    case TAG_LONG_ARRAY: {
        int len; nbt_read_int(r, &len);
        if (len >= 0) r->pos += (size_t)len * 8;
        break;
    }
    default: break;
    }
}

static void nbt_skip_tag(nbt_reader* r, unsigned char type) {
    char* name = NULL;
    if (type != TAG_END) nbt_read_string(r, &name);
    if (name) free(name);
    nbt_skip_payload(r, type);
}

/* srv_entry is declared in llcl.h */

static int nbt_parse_servers(const unsigned char* data, size_t size, srv_entry** out, int* n) {
    nbt_reader r; r.data = data; r.size = size; r.pos = 0;
    unsigned char root_type; if (!nbt_read_byte(&r, &root_type) || root_type != TAG_COMPOUND) return 0;
    char* root_name = NULL; nbt_read_string(&r, &root_name); free(root_name);
    srv_entry* arr = NULL; int cnt = 0, cap = 0;
    while (1) {
        unsigned char tag; if (!nbt_read_byte(&r, &tag)) { return 0; }
        if (tag == TAG_END) break;
        char* key = NULL; if (!nbt_read_string(&r, &key)) { free(key); return 0; }
        if (strcmp(key, "servers") == 0 && tag == TAG_LIST) {
            unsigned char child_type; int count;
            if (!nbt_read_byte(&r, &child_type) || child_type != TAG_COMPOUND) { free(key); return 0; }
            if (!nbt_read_int(&r, &count)) { free(key); return 0; }
            for (int i = 0; i < count; i++) {
                srv_entry e; memset(&e, 0, sizeof(e));
                while (1) {
                    unsigned char ft; if (!nbt_read_byte(&r, &ft)) { free(key); return 0; }
                    if (ft == TAG_END) break;
                    char* fname = NULL; if (!nbt_read_string(&r, &fname)) { free(key); return 0; }
                    if (strcmp(fname, "name") == 0 && ft == TAG_STRING) {
                        char* v = NULL; nbt_read_string(&r, &v); if (v) { e.name = u2w(v); free(v); }
                    } else if (strcmp(fname, "ip") == 0 && ft == TAG_STRING) {
                        char* v = NULL; nbt_read_string(&r, &v); if (v) { e.ip = u2w(v); free(v); }
                    } else if (strcmp(fname, "hidden") == 0 && ft == TAG_BYTE) {
                        unsigned char v; nbt_read_byte(&r, &v); e.hidden = v;
                    } else if (strcmp(fname, "icon") == 0 && ft == TAG_STRING) {
                        char* v = NULL; nbt_read_string(&r, &v); if (v) { e.icon = u2w(v); free(v); }
                    } else {
                        nbt_skip_payload(&r, ft);
                    }
                    free(fname);
                }
                if (e.name && e.ip) {
                    if (cnt == cap) { cap = cap ? cap * 2 : 8; arr = (srv_entry*)xrealloc(arr, (size_t)cap * sizeof(srv_entry)); }
                    arr[cnt++] = e;
                }
            }
        } else {
            nbt_skip_payload(&r, tag);
        }
        free(key);
    }
    *out = arr; *n = cnt;
    return 1;
}

/* 释放服务器列表 */
void server_list_free(srv_entry* e, int n) {
    if (!e) return;
    for (int i = 0; i < n; i++) { free(e[i].name); free(e[i].ip); free(e[i].icon); }
    free(e);
}

/* ---------------- 读取/写入 servers.dat ---------------- */
int servers_load(const wchar_t* gamedir, srv_entry** out, int* n) {
    wchar_t* path = path_join3(gamedir, L"servers.dat", L"");
    if (!file_exists(path)) { free(path); *out = NULL; *n = 0; return 1; }
    FILE* f = _wfopen(path, L"rb"); free(path);
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }
    unsigned char* gz = (unsigned char*)xalloc((size_t)sz);
    fread(gz, 1, (size_t)sz, f); fclose(f);
    unsigned char* nbt = NULL; size_t nbt_len = 0;
    int ok = gzip_decompress(gz, (size_t)sz, &nbt, &nbt_len);
    free(gz);
    if (!ok) return 0;
    int rc = nbt_parse_servers(nbt, nbt_len, out, n);
    free(nbt);
    return rc;
}

static void nbt_write_byte(unsigned char** buf, size_t* pos, size_t* cap, unsigned char v) {
    if (*pos >= *cap) { *cap = *cap * 2 + 1024; *buf = (unsigned char*)xrealloc(*buf, *cap); }
    (*buf)[(*pos)++] = v;
}
static void nbt_write_short(unsigned char** buf, size_t* pos, size_t* cap, short v) {
    nbt_write_byte(buf, pos, cap, (unsigned char)(v >> 8));
    nbt_write_byte(buf, pos, cap, (unsigned char)(v & 0xFF));
}
static void nbt_write_int(unsigned char** buf, size_t* pos, size_t* cap, int v) {
    nbt_write_byte(buf, pos, cap, (unsigned char)(v >> 24));
    nbt_write_byte(buf, pos, cap, (unsigned char)((v >> 16) & 0xFF));
    nbt_write_byte(buf, pos, cap, (unsigned char)((v >> 8) & 0xFF));
    nbt_write_byte(buf, pos, cap, (unsigned char)(v & 0xFF));
}
static void nbt_write_string(unsigned char** buf, size_t* pos, size_t* cap, const char* s) {
    int len = (int)strlen(s);
    nbt_write_short(buf, pos, cap, (short)len);
    if (len > 0) {
        if (*pos + len >= *cap) { *cap = *cap * 2 + len + 1024; *buf = (unsigned char*)xrealloc(*buf, *cap); }
        memcpy(*buf + *pos, s, (size_t)len);
        *pos += len;
    }
}

static void nbt_write_server_entry(unsigned char** buf, size_t* pos, size_t* cap, const srv_entry* e) {
    /* Compound start (implicit) */
    /* name */
    char* n = w2u(e->name); nbt_write_byte(buf, pos, cap, TAG_STRING); nbt_write_string(buf, pos, cap, "name"); nbt_write_string(buf, pos, cap, n); free(n);
    /* ip */
    char* ip = w2u(e->ip); nbt_write_byte(buf, pos, cap, TAG_STRING); nbt_write_string(buf, pos, cap, "ip"); nbt_write_string(buf, pos, cap, ip); free(ip);
    /* hidden */
    nbt_write_byte(buf, pos, cap, TAG_BYTE); nbt_write_string(buf, pos, cap, "hidden"); nbt_write_byte(buf, pos, cap, e->hidden ? 1 : 0);
    /* icon (optional) */
    if (e->icon && e->icon[0]) {
        char* ic = w2u(e->icon);
        nbt_write_byte(buf, pos, cap, TAG_STRING);
        nbt_write_string(buf, pos, cap, "icon");
        nbt_write_string(buf, pos, cap, ic);
        free(ic);
    }
    /* end compound */
    nbt_write_byte(buf, pos, cap, TAG_END);
}

int servers_save(const wchar_t* gamedir, const srv_entry* e, int n) {
    /* 构造 NBT: { "servers": [ ... ] } */
    unsigned char* buf = (unsigned char*)xalloc(8192);
    size_t pos = 0, cap = 8192;
    nbt_write_byte(&buf, &pos, &cap, TAG_COMPOUND);
    nbt_write_string(&buf, &pos, &cap, "");
    nbt_write_byte(&buf, &pos, &cap, TAG_LIST);
    nbt_write_string(&buf, &pos, &cap, "servers");
    nbt_write_byte(&buf, &pos, &cap, TAG_COMPOUND); /* child type */
    nbt_write_int(&buf, &pos, &cap, n);
    for (int i = 0; i < n; i++) nbt_write_server_entry(&buf, &pos, &cap, &e[i]);
    nbt_write_byte(&buf, &pos, &cap, TAG_END); /* end list */
    nbt_write_byte(&buf, &pos, &cap, TAG_END); /* end root */
    /* gzip 压缩 */
    unsigned char* gz = NULL; size_t gz_len = 0;
    if (!gzip_compress(buf, pos, &gz, &gz_len)) { free(buf); return 0; }
    free(buf);
    wchar_t* path = path_join3(gamedir, L"servers.dat", L"");
    FILE* f = _wfopen(path, L"wb");
    if (!f) { free(gz); free(path); return 0; }
    fwrite(gz, 1, gz_len, f);
    fclose(f);
    free(gz); free(path);
    return 1;
}

/* ---------------- 公共 API：调用时需传完整路径 ---------------- */
int servers_load_path(const wchar_t* path, srv_entry** out, int* n) {
    if (!file_exists(path)) { *out = NULL; *n = 0; return 1; }
    FILE* f = _wfopen(path, L"rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }
    unsigned char* gz = (unsigned char*)xalloc((size_t)sz);
    fread(gz, 1, (size_t)sz, f); fclose(f);
    unsigned char* nbt = NULL; size_t nbt_len = 0;
    int ok = gzip_decompress(gz, (size_t)sz, &nbt, &nbt_len);
    free(gz);
    if (!ok) return 0;
    int rc = nbt_parse_servers(nbt, nbt_len, out, n);
    free(nbt);
    return rc;
}

int servers_save_path(const wchar_t* path, const srv_entry* e, int n) {
    unsigned char* buf = (unsigned char*)xalloc(8192);
    size_t pos = 0, cap = 8192;
    nbt_write_byte(&buf, &pos, &cap, TAG_COMPOUND);
    nbt_write_string(&buf, &pos, &cap, "");
    nbt_write_byte(&buf, &pos, &cap, TAG_LIST);
    nbt_write_string(&buf, &pos, &cap, "servers");
    nbt_write_byte(&buf, &pos, &cap, TAG_COMPOUND);
    nbt_write_int(&buf, &pos, &cap, n);
    for (int i = 0; i < n; i++) nbt_write_server_entry(&buf, &pos, &cap, &e[i]);
    nbt_write_byte(&buf, &pos, &cap, TAG_END);
    nbt_write_byte(&buf, &pos, &cap, TAG_END);
    unsigned char* gz = NULL; size_t gz_len = 0;
    if (!gzip_compress(buf, pos, &gz, &gz_len)) { free(buf); return 0; }
    free(buf);
    FILE* f = _wfopen(path, L"wb");
    if (!f) { free(gz); return 0; }
    fwrite(gz, 1, gz_len, f);
    fclose(f);
    free(gz);
    return 1;
}

/* ---------------- 启动参数：--server <ip:port> ---------------- */
static int srv_add_arg(const srv_entry* e, wsa* args) {
    if (!e || !e->ip || !e->ip[0]) return 0;
    wchar_t* arg = wcat3(L"--server", L" ", e->ip);
    wsa_addown(args, arg);
    return 1;
}