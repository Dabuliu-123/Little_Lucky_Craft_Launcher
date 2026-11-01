/* LLcl - download.c : 游戏/资源下载（WinINet，Win7 兼容，零第三方依赖）
 * 下载版本清单 → 版本 JSON → client jar / libraries / natives / assets。
 * libraries/assets 多线程并发（每线程独立 WinINet session），BMCLAPI 镜像优先（国内友好）。
 */
#include "llcl.h"
#define JSMN_HEADER /* only declarations; implementation lives in jsonx.c */
#include "jsmn.h"
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

#define DL_BUFSZ 65536
#define DL_FILEBUF 32768       /* 文件下载单块缓冲（快速 CDN 上 32KB 足够，并发时省一半缓冲内存） */
#define DL_TIMEOUT 20000
#define DL_THREADS 8          /* 默认并发下载线程数（可由 g_dl_threads / LLCL_DL_THREADS 覆盖） */
int g_dl_threads = 8;          /* 全局下载并发数，由设置界面 g_cfg.dl_threads 同步 */
#define BMCLAPI_ROOT L"https://bmclapi2.bangbang93.com"
#define BMCLAPI_MANIFEST BMCLAPI_ROOT L"/mc/game/version_manifest_v2.json"
#define MOJANG_MANIFEST L"https://launchermeta.mojang.com/mc/game/version_manifest_v2.json"
#define PISTON_MANIFEST L"https://piston-meta.mojang.com/mc/game/version_manifest_v2.json"

/* ---------------- HTTP (WinINet) ---------------- */
static uint64_t file_size(const wchar_t* p) {
    WIN32_FILE_ATTRIBUTE_DATA fd;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &fd)) return 0;
    return ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
}

int net_init(HINTERNET* inet) {
    *inet = InternetOpenW(L"LLcl/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!*inet) return 0;
    DWORD tmo = DL_TIMEOUT;
    InternetSetOptionW(*inet, INTERNET_OPTION_CONNECT_TIMEOUT, &tmo, sizeof(tmo));
    InternetSetOptionW(*inet, INTERNET_OPTION_SEND_TIMEOUT, &tmo, sizeof(tmo));
    InternetSetOptionW(*inet, INTERNET_OPTION_RECEIVE_TIMEOUT, &tmo, sizeof(tmo));
    /* 放宽每服务器并发连接数（WinINet 默认 HTTP/1.1 仅 2~6 路，并发下载会被自己限流） */
    DWORD mcs = 32;   /* 放宽到 32 路，保证选 16 线程时不被 WinINet 自身限流 */
    InternetSetOptionW(*inet, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &mcs, sizeof(mcs));
    InternetSetOptionW(*inet, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER, &mcs, sizeof(mcs));
    return 1;
}

/* GET url → 全部字节。返回 1 传输成功（HTTP 状态写入 *status） */
int net_get(HINTERNET inet, const wchar_t* url, unsigned char** out, size_t* out_len, int* status) {
    *out = NULL; *out_len = 0;
    HINTERNET h = InternetOpenUrlW(inet, url, NULL, 0,
                                   INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD |
                                   INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION, 0);
    if (!h) return 0;
    DWORD st = 0, stl = sizeof(st);
    if (!HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &st, &stl, NULL)) st = 0;
    /* 已知 Content-Length 时一次性分配，避免反复 realloc 产生堆碎片/堆段膨胀 */
    size_t cap = 65536;
    {
        char clh[64]; DWORD cll = sizeof(clh), idx = 0;
        if (HttpQueryInfoA(h, HTTP_QUERY_CONTENT_LENGTH, clh, &cll, &idx)) {
            long long cl = _atoi64(clh);
            if (cl > 0 && cl < 256 * 1024 * 1024) cap = (size_t)cl + 1;
        }
    }
    size_t n = 0;
    unsigned char* buf = (unsigned char*)xalloc(cap);
    unsigned char* tmp = (unsigned char*)xalloc(DL_BUFSZ);  /* 堆缓冲，省栈 */
    DWORD rd = 0;
    while (InternetReadFile(h, tmp, DL_BUFSZ, &rd) && rd > 0) {
        if (n + rd > cap) {
            cap = cap * 2 + rd;
            buf = (unsigned char*)xrealloc(buf, cap);
        }
        memcpy(buf + n, tmp, rd);
        n += rd;
        rd = 0;
    }
    free(tmp);
    InternetCloseHandle(h);
    if (st != 200) { free(buf); *status = (int)st; return 1; }
    *out = buf; *out_len = n;
    *status = (int)st;
    return 1;
}

/* GET url → 写文件（递归建目录）。支持断点续传 + 自动重试，应对不稳定网络（连接中断后从已下载处继续）。
 * cb 回调字节进度（cur,total,ctx）。total 未知时为 0。 */
int net_download_file(HINTERNET inet, const wchar_t* url, const wchar_t* dest,
                      void(*cb)(uint64_t cur, uint64_t total, void* ctx), void* ctx) {
    /* 先确保目标目录存在（只做一次） */
    wchar_t* dird = wdup(dest);
    wchar_t* slash = wcsrchr(dird, L'\\');
    wchar_t* fwd = wcsrchr(dird, L'/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    if (slash) { *slash = 0; if (!dir_exists(dird)) ensure_dir_rec(dird); }
    free(dird);

    unsigned char* buf = (unsigned char*)xalloc(DL_FILEBUF);
    uint64_t total = 0;
    int done = 0;
    for (int attempt = 0; attempt < 8 && !done; attempt++) {
        uint64_t have = file_size(dest);
        wchar_t rq[64] = L"";
        if (have > 0) _snwprintf_s(rq, _countof(rq), _TRUNCATE, L"Range: bytes=%llu-\r\n",
                                   (unsigned long long)have);
        HINTERNET h = InternetOpenUrlW(inet, url, rq, (DWORD)-1L,
                                       INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD |
                                       INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION, 0);
        if (!h) { Sleep(600); continue; }
        DWORD st = 0, stl = sizeof(st);
        if (!HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &st, &stl, NULL)) st = 0;
        if (st == 416 && file_size(dest) > 0) {
            /* 已有文件不少于服务器资源：Range 被拒绝说明它很可能已经完整，
               旧实现会重试 8 次后删掉这个完好的文件。 */
            InternetCloseHandle(h);
            done = 1;
            break;
        }
        if (st != 200 && st != 206) { InternetCloseHandle(h); Sleep(600); continue; }
        /* 总大小：206 时 Content-Length 是剩余字节，需加已下载；200 时服务器忽略 Range 从头 */
        if (total == 0) {
            DWORD tl = sizeof(total), got = 0; char ch[64];
            if (HttpQueryInfoA(h, HTTP_QUERY_CONTENT_LENGTH, ch, &tl, &got))
                total = (uint64_t)_atoi64(ch) + (st == 206 ? have : 0);
        }
        if (st == 200) have = 0;
        FILE* f = _wfopen(dest, (st == 206) ? L"ab" : L"wb");
        if (!f) { InternetCloseHandle(h); continue; }
        DWORD rd = 0; int ok = 1;
        while (InternetReadFile(h, buf, DL_FILEBUF, &rd) && rd > 0) {
            if (fwrite(buf, 1, rd, f) != rd) { ok = 0; break; }
            have += rd;
            if (cb) cb(have, total, ctx);
            rd = 0;
        }
        fclose(f);
        InternetCloseHandle(h);
        if (!ok) { Sleep(900); continue; }     /* 读/写中断，下一轮从断点继续 */
        if (total == 0 || have >= total) done = 1;
        else Sleep(600);
    }
    free(buf);
    if (!done) { DeleteFileW(dest); return 0; }
    return 1;
}

/* ---------------- 版本清单 ---------------- */
void dl_list_free(dl_list* l) {
    for (int i = 0; i < l->n; i++) { free(l->v[i].id); free(l->v[i].url); }
    free(l->v); l->v = NULL; l->n = l->cap = 0;
}

/* 追加解析一份版本清单（自动去重）。返回新增条数，-1 表示格式错误。 */
static int manifest_append(dl_list* out, const char* js, size_t dlen) {
    jsmntok_t* t = NULL;
    int ntok = 0;
    if (!json_parse_mem(js, dlen, (struct jsmntok**)&t, &ntok) || ntok < 1 || t[0].type != JSMN_OBJECT) {
        free(t);
        return -1;
    }
    const jsmntok_t* vers_tok = jm(js, t, "versions");
    if (!vers_tok || vers_tok->type != JSMN_ARRAY) {
        free(t);
        return -1;
    }
    int added = 0;
    const jsmntok_t* arr = vers_tok + 1;
    for (int i = 0; i < vers_tok->size; i++) {
        const jsmntok_t* obj = arr;
        arr = tok_skip(obj);
        if (obj->type != JSMN_OBJECT) continue;
        const jsmntok_t* idt = jm(js, obj, "id");
        if (!idt || idt->type != JSMN_STRING) continue;
        wchar_t* id = jwstrdup(js, idt);
        if (!id) continue;
        const jsmntok_t* urlt = jm(js, obj, "url");
        wchar_t* url = (urlt && urlt->type == JSMN_STRING) ? jwstrdup(js, urlt) : NULL;
        int dup = 0;
        for (int k = 0; k < out->n; k++) {
            if (weq(out->v[k].id, id)) {
                dup = 1;
                /* 后到的官方清单覆盖 BMCL 清单里的 URL（官方 URL 更权威） */
                if (url) { free(out->v[k].url); out->v[k].url = url; url = NULL; }
                break;
            }
        }
        if (dup) { free(id); free(url); continue; }
        if (out->n >= out->cap) {
            out->cap = out->cap ? out->cap * 2 : 128;
            out->v = (dl_ver*)xrealloc(out->v, (size_t)out->cap * sizeof(dl_ver));
        }
        out->v[out->n].id = id;
        out->v[out->n].url = url;
        out->n++;
        added++;
    }
    free(t);
    return added;
}

/* 拉取版本清单（BMCLAPI + 官方合并）。
 * 关键：BMCLAPI 的清单可能滞后（新版本如 26.3 已经发布但镜像还没同步），
 * 旧实现只要 BMCL 清单请求成功就停止，导致查找新版本时永远找不到。
 * 现在两个源都尝试，以官方清单补齐，并按 id 去重。 */
int dl_fetch_manifest(dl_list* out, wchar_t* err, size_t errlen) {
    HINTERNET inet;
    if (!net_init(&inet)) { _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败"); return 0; }
    const wchar_t* urls[3] = { BMCLAPI_MANIFEST, MOJANG_MANIFEST, PISTON_MANIFEST };
    int any_ok = 0;
    for (int i = 0; i < 3; i++) {
        unsigned char* data = NULL; size_t dlen = 0; int st = 0;
        if (net_get(inet, urls[i], &data, &dlen, &st) && st == 200 && data && dlen > 0) {
            if (manifest_append(out, (const char*)data, dlen) >= 0) any_ok = 1;
        }
        free(data);
    }
    InternetCloseHandle(inet);
    if (!any_ok || out->n <= 0) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"无法获取版本清单（网络不可用或源不可达）");
        return 0;
    }
    return 1;
}

/* ---------------- 镜像与并发下载 ---------------- */
static wchar_t* bmclapi_mirror(const wchar_t* url) {
    const wchar_t* libs = L"https://libraries.minecraft.net/";
    const wchar_t* res  = L"https://resources.download.minecraft.net/";
    const wchar_t* fab  = L"https://maven.fabricmc.net/";
    if (wcsncmp(url, libs, wcslen(libs)) == 0)
        return wcat3(BMCLAPI_ROOT L"/libraries/", url + wcslen(libs), L"");
    if (wcsncmp(url, res, wcslen(res)) == 0)
        return wcat3(BMCLAPI_ROOT L"/assets/", url + wcslen(res), L"");
    /* Fabric 加载器及其依赖：BMCLAPI /maven 全量代理（国内快），失败回退官方 */
    if (wcsncmp(url, fab, wcslen(fab)) == 0)
        return wcat3(BMCLAPI_ROOT L"/maven/", url + wcslen(fab), L"");
    return NULL;
}

/* dl_job 定义在 llcl.h（modpack 复用） */

/* 单文件：BMCLAPI 镜像优先（国内 CDN 快），失败回退官方源 */
static int dl_fetch_with(HINTERNET inet, const wchar_t* url, const wchar_t* dest,
                         void(*cb)(uint64_t, uint64_t, void*), void* ctx) {
    wchar_t* mir = bmclapi_mirror(url);
    if (mir) {
        int ok = net_download_file(inet, mir, dest, cb, ctx);
        free(mir);
        if (ok) return 1;
    }
    return net_download_file(inet, url, dest, cb, ctx);
}

typedef struct {
    dl_job* jobs;
    size_t n;
    HINTERNET inet;          /* 共享单 WinINet session（主线程创建，所有 worker 复用） */
    volatile LONG idx;
    volatile LONG fail_taken;
    wchar_t fail_name[256];
    dl_progress* prog;
} dl_pool;

typedef struct { dl_progress* prog; LONG64 last; } dl_cbctx;

static void file_cb(uint64_t cur, uint64_t total, void* ctx) {
    (void)total;
    dl_cbctx* c = (dl_cbctx*)ctx;
    LONG64 delta = (LONG64)cur - c->last;
    if (delta > 0) InterlockedAdd64(&c->prog->bytes_done, delta);
    c->last = (LONG64)cur;
}

static DWORD WINAPI dl_worker(LPVOID p) {
    dl_pool* q = (dl_pool*)p;
    HINTERNET inet = q->inet;
    dl_cbctx cbc;
    cbc.prog = q->prog;
    for (;;) {
        LONG i = InterlockedIncrement(&q->idx) - 1;
        if ((size_t)i >= q->n) break;
        dl_job* job = &q->jobs[i];
        if (q->prog) {
            wcsncpy_s(q->prog->cur, 256, job->name ? job->name : L"", _TRUNCATE);
            InterlockedIncrement(&q->prog->active);
        }
        cbc.last = 0;
        int ok = dl_fetch_with(inet, job->url, job->dest, file_cb, &cbc);
        /* 已知期望大小时再做一次落盘校验：镜像返回 200 错误页时，文件存在
           但内容不是 jar/资源，后面启动才会报 NoClassDefFoundError/资源缺失。 */
        if (ok && job->size > 0 && file_size(job->dest) != job->size) {
            _wremove(job->dest);
            ok = 0;
        }
        if (!ok) {
            if (q->prog) InterlockedIncrement(&q->prog->failed);
            if (InterlockedCompareExchange(&q->fail_taken, 1, 0) == 0 && job->name)
                wcsncpy_s(q->fail_name, 256, job->name, _TRUNCATE);
        }
        if (q->prog) {
            InterlockedDecrement(&q->prog->active);
            InterlockedIncrement(&q->prog->files_done);
        }
    }
    return 0;
}

/* 并发执行一组下载任务；返回 1 全部成功。
 * 所有 worker 共享一个 WinINet session（WinINet 线程安全，请求句柄各自独立），
 * 避免"每线程一个 InternetOpen"叠加 wininet 基础开销，从而可在小内存下提高并发。 */
int dl_run_jobs(dl_job* jobs, size_t n, dl_progress* prog, wchar_t* err, size_t errlen) {
    if (n == 0) return 1;
    HINTERNET inet;
    if (!net_init(&inet)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败");
        return 0;
    }
    dl_pool pool;
    memset(&pool, 0, sizeof(pool));
    pool.jobs = jobs; pool.n = n; pool.prog = prog; pool.inet = inet;
    if (prog) {
        InterlockedExchange(&prog->files_total, (LONG)n);
        InterlockedExchange(&prog->failed, 0);
    }
    /* 并发数：设置界面 g_dl_threads（默认 8），LLCL_DL_THREADS 环境变量可临时覆盖 */
    int maxthreads = (g_dl_threads == 16) ? 16 : 8;
    {
        wchar_t ev[32];
        if (GetEnvironmentVariableW(L"LLCL_DL_THREADS", ev, 32) > 0) {
            int v = _wtoi(ev);
            if (v > 0 && v <= 64) maxthreads = v;
        }
    }
    int nthreads = (n < maxthreads) ? (int)n : maxthreads;
    HANDLE* th = (HANDLE*)xalloc((size_t)nthreads * sizeof(HANDLE));
    int created = 0;
    for (int i = 0; i < nthreads; i++) {
        th[i] = CreateThread(NULL, 49152, dl_worker, &pool, 0, NULL);
        if (th[i]) created++;
    }
    if (created == 0) {
        /* 线程创建失败：退回本线程串行下载 */
        dl_worker(&pool);
    } else {
        WaitForMultipleObjects(created, th, TRUE, INFINITE);
    }
    for (int i = 0; i < nthreads; i++) if (th[i]) CloseHandle(th[i]);
    free(th);
    InternetCloseHandle(inet);
    mem_trim();   /* 下载结束：压缩各堆并归还物理内存，让完成态回落 */
    LONG failed = prog ? InterlockedCompareExchange(&prog->failed, 0, 0) : 0;
    if (failed > 0) {
        _snwprintf_s(err, errlen, _TRUNCATE,
                     L"%d 个文件下载失败，首个失败: %s（可重试，已下载的文件会自动跳过）",
                     (int)failed, pool.fail_name[0] ? pool.fail_name : L"未知");
        return 0;
    }
    return 1;
}

static void jobs_add(dl_job** jobs, size_t* n, size_t* cap,
                     const wchar_t* url, const wchar_t* dest, const wchar_t* name,
                     uint64_t size) {
    if (*n >= *cap) {
        *cap = *cap ? *cap * 2 : 128;
        *jobs = (dl_job*)xrealloc(*jobs, *cap * sizeof(dl_job));
    }
    (*jobs)[*n].url = wdup(url);
    (*jobs)[*n].dest = wdup(dest);
    (*jobs)[*n].name = wdup(name ? name : L"");
    (*jobs)[*n].size = size;
    (*n)++;
}

static void jobs_free(dl_job* jobs, size_t n) {
    for (size_t i = 0; i < n; i++) { free(jobs[i].url); free(jobs[i].dest); free(jobs[i].name); }
    free(jobs);
}

/* 改写版本 JSON 顶层 "id" 字段（自定义版本名时用）。返回 1 成功。 */
/* 自定义版本名：改写顶层 id，并注入 llcl_base_version 记录原版版本号
 * （加载器/Mod 平台按原版版本号查询，自定义名无法直接查询）。 */
static int rewrite_json_id(const wchar_t* json_path, const wchar_t* new_id,
                           const wchar_t* base_version) {
    char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
    if (!json_load_file(json_path, &js, &t, &nt) || !js) { json_free(js, t); return 0; }
    const jsmntok_t* idt = jm(js, t, "id");
    if (!idt) { json_free(js, t); return 0; }
    char* nid = w2u(new_id);
    if (!nid) { json_free(js, t); return 0; }
    /* 构造插入字段（紧跟 '{'，跳过空白） */
    char* ins = NULL; size_t inslen = 0;
    int insertAt = 1;
    while (insertAt < (int)strlen(js) &&
           (js[insertAt] == ' ' || js[insertAt] == '\t' ||
            js[insertAt] == '\n' || js[insertAt] == '\r')) insertAt++;
    if (base_version && base_version[0]) {
        char* bv = w2u(base_version);
        ins = (char*)xalloc(strlen(bv) + 32);
        int n = _snprintf_s(ins, strlen(bv) + 32, _TRUNCATE,
                            "\"llcl_base_version\":\"%s\",", bv);
        inslen = (n > 0) ? (size_t)n : 0;
        free(bv);
    }
    size_t pre = (size_t)idt->start, suf = (size_t)idt->end, total = strlen(js);
    size_t nidlen = strlen(nid);
    size_t seg1 = (size_t)insertAt, seg2 = pre - seg1, seg4 = total - suf;
    char* out = (char*)xalloc(seg1 + inslen + seg2 + nidlen + seg4 + 1);
    char* p = out;
    memcpy(p, js, seg1); p += seg1;
    if (inslen) { memcpy(p, ins, inslen); p += inslen; }
    memcpy(p, js + insertAt, seg2); p += seg2;
    memcpy(p, nid, nidlen); p += nidlen;
    memcpy(p, js + suf, seg4); p += seg4;
    *p = 0;
    FILE* f = _wfopen(json_path, L"wb");
    if (f) { fwrite(out, 1, p - out, f); fclose(f); }
    free(out); free(nid); free(ins);
    json_free(js, t);
    return 1;
}

/* ---------------- 版本安装 ---------------- */
/* dl_progress 定义见 llcl.h */

/* 从本地版本 JSON 取 downloads.client.url / size 与 assetIndex.url */
static void version_urls(const wchar_t* json_path, wchar_t** client_url, uint64_t* client_size,
                         wchar_t** assets_url, uint64_t* assets_size, wchar_t** assets_sha1) {
    char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
    if (!json_load_file(json_path, &js, &t, &nt) || !js || nt < 1) { free(js); free(t); return; }
    const jsmntok_t* dl = jm(js, t, "downloads");
    if (dl) {
        const jsmntok_t* cl = jm(js, dl, "client");
        if (cl) {
            const jsmntok_t* u = jm(js, cl, "url"); if (u && u->type == JSMN_STRING) *client_url = jwstrdup(js, u);
            const jsmntok_t* sz = jm(js, cl, "size"); if (sz && sz->type == JSMN_PRIMITIVE) *client_size = (uint64_t)strtoull(js + sz->start, NULL, 10);
        }
    }
    const jsmntok_t* ai = jm(js, t, "assetIndex");
    if (ai) {
        const jsmntok_t* u = jm(js, ai, "url");
        if (u && u->type == JSMN_STRING) *assets_url = jwstrdup(js, u);
        const jsmntok_t* sz = jm(js, ai, "size");
        if (sz && sz->type == JSMN_PRIMITIVE) *assets_size = (uint64_t)strtoull(js + sz->start, NULL, 10);
        const jsmntok_t* sh = jm(js, ai, "sha1");
        if (sh && sh->type == JSMN_STRING) *assets_sha1 = jwstrdup(js, sh);
    }
    free(js); free(t);
}

/* 下载并安装一个版本（版本 JSON + client + libraries/natives + assets）。返回 1 成功。
   custom_name 非空时，安装到 versions/<custom_name>/ 并改写 JSON id（自定义版本名）。 */
int dl_install_version(const wchar_t* gamedir, const wchar_t* version_id,
                       const wchar_t* custom_name, int skip_assets,
                       dl_progress* prog, wchar_t* err, size_t errlen) {
    HINTERNET inet;
    if (!net_init(&inet)) { _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败"); return 0; }
    int rc = 0;
    {
        const wchar_t* check = (custom_name && custom_name[0]) ? custom_name : version_id;
        const wchar_t* bad = L"\\/:*?\"<>|";
        int badc = !check || !*check;
        for (const wchar_t* p = check; p && *p && !badc; p++) {
            if (*p < 0x20 || *p == L'\\' || *p == L'/') badc = 1;
            for (const wchar_t* q = bad; *q; q++) if (*p == *q) { badc = 1; break; }
        }
        if (check && wcsstr(check, L"..")) badc = 1;
        if (badc) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"版本名包含非法路径字符");
            InternetCloseHandle(inet);
            return 0;
        }
    }
    const wchar_t* dir_id = (custom_name && custom_name[0]) ? custom_name : version_id;
    wchar_t* vdir = path_join3(gamedir, L"versions", dir_id);
    wchar_t* json_path = path_join(vdir, wcat3(dir_id, L".json", L""));
    if (!dir_exists(vdir)) ensure_dir_rec(vdir);

    /* 1. 版本 JSON：BMCLAPI version json 优先（国内友好、免拉全量清单），
       失败/内容无效时回退到合并后的版本清单（BMCL + 官方）定位官方 URL。 */
    if (prog) { wcsncpy_s(prog->cur, 256, L"下载版本 JSON", _TRUNCATE); }
    int vjson_ok = 0;
    {
        wchar_t* bmcl = wcat3(BMCLAPI_ROOT L"/version/", version_id, L"/json");
        if (dl_fetch_with(inet, bmcl, json_path, NULL, NULL)) {
            char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
            if (json_load_file(json_path, &js, &t, &nt) && js && nt > 0) vjson_ok = 1;
            free(js); free(t);
        }
        free(bmcl);
        if (!vjson_ok) _wremove(json_path);
    }
    if (!vjson_ok) {
        dl_list man; memset(&man, 0, sizeof(man));
        wchar_t* vurl = NULL;
        if (dl_fetch_manifest(&man, err, errlen)) {
            for (int i = 0; i < man.n; i++)
                if (weq(man.v[i].id, version_id)) { vurl = wdup(man.v[i].url); break; }
        }
        if (!vurl) {
            _snwprintf_s(err, errlen, _TRUNCATE,
                         L"下载版本 JSON 失败: %s（版本清单中没有该版本，可能尚未发布/网络不可用）",
                         version_id);
            dl_list_free(&man); free(json_path); free(vdir);
            InternetCloseHandle(inet); return 0;
        }
        if (!dl_fetch_with(inet, vurl, json_path, NULL, NULL)) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"下载版本 JSON 失败: %s", version_id);
            free(vurl); dl_list_free(&man); free(json_path); free(vdir);
            InternetCloseHandle(inet); return 0;
        }
        free(vurl);
        dl_list_free(&man);
        char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
        if (!json_load_file(json_path, &js, &t, &nt) || !js || nt <= 0) {
            free(js); free(t);
            _snwprintf_s(err, errlen, _TRUNCATE, L"版本 JSON 内容无效: %s", version_id);
            free(json_path); free(vdir); InternetCloseHandle(inet); return 0;
        }
        free(js); free(t);
    }
    /* 自定义版本名：改写顶层 id 为 dir_id，并记录原版版本号（供加载器查询） */
    if (custom_name && custom_name[0])
        rewrite_json_id(json_path, dir_id, version_id);

    /* 2. 解析本地 JSON（与启动共用解析器，保证一致） */
    vinfo v; memset(&v, 0, sizeof(v));
    if (!resolve_version(gamedir, dir_id, &v)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"解析版本 %s 失败", dir_id);
        free(json_path); free(vdir); InternetCloseHandle(inet); return 0;
    }
    wchar_t* client_url = NULL, * assets_url = NULL, * assets_sha1 = NULL;
    uint64_t client_size = 0, assets_size = 0;
    version_urls(json_path, &client_url, &client_size, &assets_url, &assets_size, &assets_sha1);

    /* 3. client jar：BMCLAPI 镜像优先（国内快），缺失或大小不符则重下。
       注意：镜像/CDN 有时会用 HTTP 200 返回错误页，必须校验大小，否则会
       留下一个损坏的 client.jar，表现为“版本能安装但无法启动”。 */
    if (prog) { wcsncpy_s(prog->cur, 256, L"下载客户端主程序", _TRUNCATE); }
    wchar_t* cj = path_join(vdir, wcat3(dir_id, L".jar", L""));
    if (file_exists(cj) && client_size > 0 && file_size(cj) != client_size) _wremove(cj);
    if (!file_exists(cj)) {
        wchar_t* bc = wcat3(BMCLAPI_ROOT L"/version/", version_id, L"/client");
        int okc = net_download_file(inet, bc, cj, NULL, NULL);
        free(bc);
        if (okc && client_size > 0 && file_size(cj) != client_size) {
            _wremove(cj);
            okc = 0;
        }
        if (!okc && client_url) {
            okc = net_download_file(inet, client_url, cj, NULL, NULL);
            if (okc && client_size > 0 && file_size(cj) != client_size) {
                _wremove(cj);
                okc = 0;
            }
        }
        if (!okc) {
            _snwprintf_s(err, errlen, _TRUNCATE, L"下载客户端主程序失败: %s", version_id);
            free(client_url); free(assets_url); free(assets_sha1);
            free(cj); free(json_path); free(vdir); vinfo_free(&v); InternetCloseHandle(inet);
            return 0;
        }
    }
    free(client_url);
    free(cj);

    /* 4. 收集 libraries + natives 缺失文件任务 */
    dl_job* jobs = NULL; size_t njob = 0, jcap = 0;
    for (size_t i = 0; i < v.nlibs; i++) {
        libitem* li = &v.libs[i];
        if (file_exists(li->fullpath)) {
            if (li->size == 0 || file_size(li->fullpath) == li->size) continue;
            _wremove(li->fullpath);   /* 大小不符：视为损坏，重下 */
        }
        wchar_t* url = wcat3(L"https://libraries.minecraft.net/", li->rel, L"");
        jobs_add(&jobs, &njob, &jcap, url, li->fullpath, li->name, li->size);
        free(url);
    }

    /* 5. assets index + objects（index 先单独下载，再把缺失对象并入任务列表统一并发）。
       26.x 的客户端在缺少资源对象时会直接在启动阶段崩溃（NoSuchFileException），
       因此索引失败不能再当作“不影响启动”静默略过。 */
    int assets_failed = 0;
    if (!skip_assets && v.assets && !weq(v.assets, L"legacy") && !assets_url && !assets_sha1) {
        assets_failed = 1;
        _snwprintf_s(err, errlen, _TRUNCATE, L"版本 JSON 缺少 assetIndex 地址，无法下载资源");
    }
    if (!skip_assets && v.assets && !weq(v.assets, L"legacy")) {
        if (prog) { wcsncpy_s(prog->cur, 256, L"下载资源索引", _TRUNCATE); }
        wchar_t* idxpath = path_join3(gamedir, L"assets", L"indexes");
        ensure_dir_rec(idxpath);
        wchar_t* idxfile = path_join(idxpath, wcat3(v.assets, L".json", L""));
        int idx_ok = 0;
        /* 已有索引：大小/内容有效则跳过网络 */
        if (file_exists(idxfile) && (assets_size == 0 || file_size(idxfile) == assets_size)) {
            char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
            if (json_load_file(idxfile, &js, &t, &nt) && js && nt > 1) idx_ok = 1;
            free(js); free(t);
        }
        if (!idx_ok) {
            _wremove(idxfile);
            if (assets_url && dl_fetch_with(inet, assets_url, idxfile, NULL, NULL)) idx_ok = 1;
            /* 官方包 URL 不可达时，用 assetIndex.sha1 构造同一个包地址
               （piston-meta 与 launchermeta 两个官方域名都尝试）。 */
            if (!idx_ok && assets_sha1) {
                const wchar_t* roots[2] = {
                    L"https://piston-meta.mojang.com/v1/packages/",
                    L"https://launchermeta.mojang.com/v1/packages/"
                };
                for (int r = 0; r < 2 && !idx_ok; r++) {
                    wchar_t* u = wcat3(roots[r], assets_sha1, L"/");
                    wchar_t* u2 = wcat3(u, v.assets, L".json");
                    if (dl_fetch_with(inet, u2, idxfile, NULL, NULL)) idx_ok = 1;
                    free(u); free(u2);
                }
            }
            if (idx_ok && assets_size > 0 && file_size(idxfile) != assets_size) {
                _wremove(idxfile);
                idx_ok = 0;
            }
        }
        if (idx_ok) {
            char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
            if (json_load_file(idxfile, &js, &t, &nt) && js && nt > 1) {
                const jsmntok_t* objs = jm(js, t, "objects");
                if (objs && objs->type == JSMN_OBJECT) {
                    const jsmntok_t* arr = objs + 1;
                    int total_obj = objs->size;
                    for (int i = 0; i < total_obj; i++) {
                        const jsmntok_t* obj = arr;
                        arr = tok_skip(obj);
                        if (obj->type != JSMN_OBJECT) continue;
                        const jsmntok_t* htok = jm(js, obj, "hash");
                        if (!htok || htok->type != JSMN_STRING) continue;
                        uint64_t osz = 0;
                        const jsmntok_t* szt = jm(js, obj, "size");
                        if (szt && szt->type == JSMN_PRIMITIVE) osz = strtoull(js + szt->start, NULL, 10);
                        wchar_t* hw = jwstrdup(js, htok);
                        if (!hw) continue;
                        if (wcslen(hw) >= 2) {
                            wchar_t hpre[3];
                            hpre[0] = hw[0]; hpre[1] = hw[1]; hpre[2] = 0;
                            wchar_t* odir = path_join3(gamedir, L"assets", L"objects");
                            wchar_t* osub = path_join(odir, hpre);
                            wchar_t* opath = path_join(osub, hw);
                            if (!file_exists(opath) || (osz > 0 && file_size(opath) != osz)) {
                                wchar_t* aurl = wcat3(L"https://resources.download.minecraft.net/", hpre, L"/");
                                wchar_t* aurl2 = wcat3(aurl, hw, L"");
                                jobs_add(&jobs, &njob, &jcap, aurl2, opath, hw, osz);
                                free(aurl); free(aurl2);
                            }
                            free(odir); free(osub); free(opath);
                        }
                        free(hw);
                    }
                } else {
                    idx_ok = 0;
                }
            } else {
                idx_ok = 0;
            }
            free(js); free(t);
        }
        if (!idx_ok) {
            assets_failed = 1;
            _snwprintf_s(err, errlen, _TRUNCATE,
                         L"资源索引下载/校验失败（%s）。缺少资源会导致 26.x 启动崩溃，请重试。",
                         v.assets);
        }
        free(idxpath); free(idxfile);
    }
    free(assets_url);
    free(assets_sha1);
    InternetCloseHandle(inet);

    /* 6. 并发下载全部缺失文件 */
    if (prog) {
        wcsncpy_s(prog->cur, 256, njob ? L"并发下载文件" : L"文件已齐全", _TRUNCATE);
    }
    int ok = dl_run_jobs(jobs, njob, prog, err, errlen);
    jobs_free(jobs, njob);

    /* 校验库文件大小：镜像返回 200 错误页时，文件存在但内容是 HTML，
       启动时才会表现为 NoClassDefFoundError，必须在这里识别。 */
    if (ok) {
        int bad = 0;
        for (size_t i = 0; i < v.nlibs; i++) {
            libitem* li = &v.libs[i];
            if (!file_exists(li->fullpath)) { bad++; continue; }
            if (li->size > 0 && file_size(li->fullpath) != li->size) {
                _wremove(li->fullpath);
                bad++;
            }
        }
        if (bad > 0) {
            _snwprintf_s(err, errlen, _TRUNCATE,
                         L"%d 个库文件下载不完整或大小不符，请重试下载该版本", bad);
            ok = 0;
        }
    }
    if (!ok) {
        free(json_path); free(vdir); vinfo_free(&v);
        return 0;
    }
    if (assets_failed) {
        free(json_path); free(vdir); vinfo_free(&v);
        return 0;
    }
    /* 7. 自动检测该版本所需 Java（向后兼容：>= 所需即可），没有则一并下载（不阻断：
          游戏已安装；即便此处失败，启动游戏时 do_launch 会再次兜底提示下载） */
    {
        int needJ = v.java_major > 0 ? v.java_major : 8;
        wchar_t* have = javadl_find(needJ, 1);
        int sysHas = 0;
        if (!have) {
            javaitem* jj = NULL; int jn = 0;
            if (detect_javas(&jj, &jn)) {
                for (int i = 0; i < jn; i++)
                    if (jj[i].major >= needJ && jj[i].is64) { sysHas = 1; break; }
            }
            javas_free(jj, jn);
        }
        if (!have && !sysHas) {
            if (prog) { wcsncpy_s(prog->cur, 256, L"自动下载对应 Java", _TRUNCATE); }
            javadl_install(needJ, 1, prog, err, errlen);
        }
        free(have);
    }
    rc = 1;
    free(json_path); free(vdir); vinfo_free(&v);
    return rc;
}
