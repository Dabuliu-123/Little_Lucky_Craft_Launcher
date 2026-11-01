/* LLcl - content.c : Modrinth 内容下载（Mod/光影/资源包/整合包搜索），
 * 复用 net_* (WinINet) + jsmn，零额外依赖。下载直接存 jar/zip，流式写盘，内存占用极小。
 */
#include "llcl.h"
#define JSMN_HEADER /* only declarations; implementation lives in jsonx.c */
#include "jsmn.h"

#define MR_API L"https://api.modrinth.com/v2"
#define CT_LIMIT 25
#define CT_VER_LIMIT 40

const wchar_t* ct_type_project(ct_type t) {
    switch (t) {
        case CT_MOD:     return L"mod";
        case CT_SHADER:  return L"shader";
        case CT_RP:      return L"resourcepack";
        case CT_MODPACK: return L"modpack";
    }
    return L"mod";
}
const wchar_t* ct_type_name(ct_type t) {
    switch (t) {
        case CT_MOD:     return L"模组";
        case CT_SHADER:  return L"光影";
        case CT_RP:      return L"资源包";
        case CT_MODPACK: return L"整合包";
    }
    return L"模组";
}

/* ---------------- 搜索 ---------------- */
int ct_search(const wchar_t* query, ct_type type, const wchar_t* mcver,
              const wchar_t* loader, ct_proj** out, int* n, wchar_t* err, size_t errlen) {
    *out = NULL; *n = 0;
    HINTERNET inet;
    if (!net_init(&inet)) { _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败"); return 0; }

    /* facets: 项目类型必选，版本/加载器（分类）可选 */
    wsb f; wsb_init(&f);
    wsb_add(&f, L"[[\"project_type:");
    wsb_add(&f, ct_type_project(type));
    wsb_add(&f, L"\"]");
    if (mcver && mcver[0]) {
        wsb_add(&f, L",[\"versions:");
        wsb_add(&f, mcver);
        wsb_add(&f, L"\"]");
    }
    if (loader && loader[0]) {
        wsb_add(&f, L",[\"categories:");
        wsb_add(&f, loader);
        wsb_add(&f, L"\"]");
    }
    wsb_add(&f, L"]");

    wsb url; wsb_init(&url);
    wsb_addf(&url, MR_API L"/search?limit=%d&facets=", CT_LIMIT);
    net_urlenc(&url, f.b);
    wsb_free(&f);
    if (query && query[0]) { wsb_add(&url, L"&query="); net_urlenc(&url, query); }

    unsigned char* data = NULL; size_t dlen = 0; int st = 0;
    int got = net_get(inet, url.b, &data, &dlen, &st);
    wsb_free(&url);
    InternetCloseHandle(inet);
    if (!got || st != 200 || !data) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"搜索失败（HTTP %d）", st);
        free(data); return 0;
    }

    const char* js = (const char*)data;
    jsmntok_t* t = NULL; int nt = 0;
    if (!json_parse_mem(js, dlen, (struct jsmntok**)&t, &nt) || nt < 1) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"搜索结果格式错误");
        free(t); free(data); return 0;
    }
    const jsmntok_t* hits = jm(js, t, "hits");
    if (!hits || hits->type != JSMN_ARRAY) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"搜索结果缺少 hits");
        free(t); free(data); return 0;
    }

    ct_proj* arr = NULL; int cnt = 0;
    const jsmntok_t* cur = hits + 1;
    for (int i = 0; i < hits->size; i++) {
        const jsmntok_t* o = cur;
        cur = tok_skip(o);
        if (o->type != JSMN_OBJECT) continue;
        const jsmntok_t* idt = jm(js, o, "project_id");
        const jsmntok_t* ttt = jm(js, o, "title");
        if (!idt || !ttt) continue;
        ct_proj p;
        memset(&p, 0, sizeof(p));
        p.id = jwstrdup(js, idt);
        p.title = jwstrdup(js, ttt);
        const jsmntok_t* au = jm(js, o, "author");
        p.author = au ? jwstrdup(js, au) : wdup(L"");
        /* 描述不在列表显示，不保留以节省内存 */
        p.desc = wdup(L"");
        const jsmntok_t* dl = jm(js, o, "downloads");
        p.downloads = dl ? strtoll(js + dl->start, NULL, 10) : 0;
        if (p.id && p.title) {
            arr = (ct_proj*)xrealloc(arr, (cnt + 1) * sizeof(ct_proj));
            arr[cnt++] = p;
        } else {
            free(p.id); free(p.title); free(p.author); free(p.desc);
        }
    }
    free(t); free(data);
    *out = arr; *n = cnt;
    if (cnt == 0) _snwprintf_s(err, errlen, _TRUNCATE, L"没有找到匹配的内容");
    return cnt > 0;
}

/* ---------------- 版本列表 ---------------- */
int ct_versions(const wchar_t* project_id, const wchar_t* mcver, const wchar_t* loader,
                ct_ver** out, int* n, wchar_t* err, size_t errlen) {
    *out = NULL; *n = 0;
    HINTERNET inet;
    if (!net_init(&inet)) { _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败"); return 0; }

    wsb url; wsb_init(&url);
    wsb_add(&url, MR_API L"/project/");
    net_urlenc(&url, project_id);
    wsb_add(&url, L"/version?");
    if (mcver && mcver[0]) {
        wsb tmp; wsb_init(&tmp);
        wsb_add(&tmp, L"[\""); wsb_add(&tmp, mcver); wsb_add(&tmp, L"\"]");
        wsb_add(&url, L"game_versions=");
        net_urlenc(&url, tmp.b);
        wsb_free(&tmp);
    }
    if (loader && loader[0]) {
        wsb tmp; wsb_init(&tmp);
        wsb_add(&tmp, L"[\""); wsb_add(&tmp, loader); wsb_add(&tmp, L"\"]");
        wsb_add(&url, L"&loaders=");
        net_urlenc(&url, tmp.b);
        wsb_free(&tmp);
    }

    unsigned char* data = NULL; size_t dlen = 0; int st = 0;
    int got = net_get(inet, url.b, &data, &dlen, &st);
    wsb_free(&url);
    InternetCloseHandle(inet);
    if (!got || st != 200 || !data) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"获取版本失败（HTTP %d）", st);
        free(data); return 0;
    }

    const char* js = (const char*)data;
    jsmntok_t* t = NULL; int nt = 0;
    if (!json_parse_mem(js, dlen, (struct jsmntok**)&t, &nt) || t[0].type != JSMN_ARRAY) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"版本列表格式错误");
        free(t); free(data); return 0;
    }

    ct_ver* arr = NULL; int cnt = 0;
    const jsmntok_t* cur = t + 1;
    for (int i = 0; i < t[0].size; i++) {
        if (cnt >= CT_VER_LIMIT) break;   /* 只保留最新若干版本，省内存 */
        const jsmntok_t* o = cur;
        cur = tok_skip(o);
        if (o->type != JSMN_OBJECT) continue;
        const jsmntok_t* files = jm(js, o, "files");
        if (!files || files->type != JSMN_ARRAY || files->size < 1) continue;
        /* 优先 primary 文件，否则取第一个 */
        const jsmntok_t* chosen = files + 1;
        const jsmntok_t* fp = files + 1;
        for (int k = 0; k < files->size; k++) {
            const jsmntok_t* prim = jm(js, fp, "primary");
            if (prim && js[prim->start] == 't') { chosen = fp; break; }
            fp = tok_skip(fp);
        }
        const jsmntok_t* u = jm(js, chosen, "url");
        const jsmntok_t* fn = jm(js, chosen, "filename");
        if (!u || !fn) continue;
        ct_ver v;
        memset(&v, 0, sizeof(v));
        const jsmntok_t* idt = jm(js, o, "id");
        v.id = idt ? jwstrdup(js, idt) : wdup(L"");
        const jsmntok_t* nm = jm(js, o, "name");
        v.name = nm ? jwstrdup(js, nm) : wdup(L"");
        v.url = jwstrdup(js, u);
        v.filename = jwstrdup(js, fn);
        const jsmntok_t* sz = jm(js, chosen, "size");
        v.size = sz ? strtoull(js + sz->start, NULL, 10) : 0;
        if (v.url && v.filename) {
            arr = (ct_ver*)xrealloc(arr, (cnt + 1) * sizeof(ct_ver));
            arr[cnt++] = v;
        } else {
            free(v.id); free(v.name); free(v.url); free(v.filename);
        }
    }
    free(t); free(data);
    *out = arr; *n = cnt;
    if (cnt == 0) _snwprintf_s(err, errlen, _TRUNCATE, L"该内容没有适配此版本/加载器");
    return cnt > 0;
}

/* ---------------- 下载（流式写盘） ---------------- */
int ct_download(const ct_ver* v, const wchar_t* dest, dl_progress* prog,
                wchar_t* err, size_t errlen) {
    HINTERNET inet;
    if (!net_init(&inet)) { _snwprintf_s(err, errlen, _TRUNCATE, L"初始化网络失败"); return 0; }
    if (prog) {
        wcsncpy_s(prog->cur, 256, v->filename, _TRUNCATE);
        InterlockedIncrement(&prog->active);
    }
    int ok = net_download_file(inet, v->url, dest, NULL, NULL);
    if (prog) InterlockedDecrement(&prog->active);
    InternetCloseHandle(inet);
    if (!ok) { _snwprintf_s(err, errlen, _TRUNCATE, L"下载失败: %s", v->filename); return 0; }
    return 1;
}

/* ---------------- 释放 ---------------- */
void ct_proj_free(ct_proj* p, int n) {
    if (!p) return;
    for (int i = 0; i < n; i++) {
        free(p[i].id); free(p[i].title); free(p[i].author); free(p[i].desc);
    }
    free(p);
}
void ct_ver_free(ct_ver* v, int n) {
    if (!v) return;
    for (int i = 0; i < n; i++) {
        free(v[i].id); free(v[i].name); free(v[i].url); free(v[i].filename);
    }
    free(v);
}
