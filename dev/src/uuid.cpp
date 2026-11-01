/* LLcl - uuid.c : MD5 (RFC 1321 style) + offline player UUID (UUID v3 of "OfflinePlayer:<name>") */
#include "llcl.h"

typedef struct {
    uint32_t state[4];
    uint64_t nbytes;
    unsigned char buf[64];
    size_t buflen;
} md5_ctx;

static const uint32_t K[64] = {
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
    0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
    0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
    0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
    0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
    0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
};
static const uint32_t S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5,9,14,20, 5,9,14,20, 5,9,14,20, 5,9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void md5_block(md5_ctx* ctx, const unsigned char* p) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
               ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16) { f = (b & c) | (~b & d); g = (uint32_t)i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5u * (uint32_t)i + 1u) % 16u; }
        else if (i < 48) { f = b ^ c ^ d; g = (3u * (uint32_t)i + 5u) % 16u; }
        else { f = c ^ (b | ~d); g = (7u * (uint32_t)i) % 16u; }
        uint32_t tmp = d;
        d = c; c = b;
        b = b + rol(a + f + K[i] + M[g], (int)S[i]);
        a = tmp;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
}

void md5_of(const unsigned char* data, size_t len, unsigned char digest[16]) {
    md5_ctx ctx;
    ctx.state[0] = 0x67452301u; ctx.state[1] = 0xefcdab89u;
    ctx.state[2] = 0x98badcfeu; ctx.state[3] = 0x10325476u;
    ctx.nbytes = 0; ctx.buflen = 0;
    size_t off = 0;
    while (off < len) {
        ctx.buf[ctx.buflen++] = data[off++];
        if (ctx.buflen == 64) { md5_block(&ctx, ctx.buf); ctx.nbytes += 64; ctx.buflen = 0; }
    }
    uint64_t bitlen = (ctx.nbytes + ctx.buflen) * 8;
    ctx.buf[ctx.buflen++] = 0x80;
    if (ctx.buflen > 56) {
        while (ctx.buflen < 64) ctx.buf[ctx.buflen++] = 0;
        md5_block(&ctx, ctx.buf);
        ctx.buflen = 0;
    }
    while (ctx.buflen < 56) ctx.buf[ctx.buflen++] = 0;
    for (int i = 0; i < 8; i++) ctx.buf[56 + i] = (unsigned char)(bitlen >> (8 * i));
    md5_block(&ctx, ctx.buf);
    for (int i = 0; i < 4; i++) {
        digest[i * 4] = (unsigned char)(ctx.state[i]);
        digest[i * 4 + 1] = (unsigned char)(ctx.state[i] >> 8);
        digest[i * 4 + 2] = (unsigned char)(ctx.state[i] >> 16);
        digest[i * 4 + 3] = (unsigned char)(ctx.state[i] >> 24);
    }
}

/* Offline UUID = UUID v3 (MD5) of "OfflinePlayer:<name>", Java-compatible */
void uuid_offline(const wchar_t* player, wchar_t* out, size_t outcap) {
    char* u8 = w2u(player);
    if (!u8) u8 = udup("Player");
    char* pre = w2u(L"OfflinePlayer:");
    size_t pl = strlen(pre), ul = strlen(u8);
    char* s = (char*)xalloc(pl + ul + 1);
    memcpy(s, pre, pl);
    memcpy(s + pl, u8, ul + 1);
    unsigned char d[16];
    md5_of((const unsigned char*)s, pl + ul, d);
    /* version 3, RFC 4122 variant */
    d[6] = (unsigned char)((d[6] & 0x0F) | 0x30);
    d[8] = (unsigned char)((d[8] & 0x3F) | 0x80);
    wchar_t b[37];
    _snwprintf_s(b, 37, _TRUNCATE,
                 L"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                 d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
    wcscpy_s(out, outcap, b);
    free(s); free(pre); free(u8);
}
