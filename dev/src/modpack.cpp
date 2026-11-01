/* LLcl - modpack.cpp : Modrinth .mrpack 整合包安装（C++，仍复用 Win32/WinINet）。
 *
 * 流程：解压 mrpack 到临时目录 → 解析 modrinth.index.json → 安装游戏版本
 *      → 安装 Fabric/Quilt/Forge/NeoForge 加载器 → 并发下载 mods → 解压 overrides → 清理。
 * 开启版本隔离时，mods/配置/overrides 写入新加载器版本的 instance 目录。
 */
#include "llcl.h"
#define JSMN_HEADER
#include "jsmn.h"

/* ---------------- 数据结构 ---------------- */
typedef struct {
    wchar_t* path;   /* 相对游戏目录，如 mods\xxx.jar */
    wchar_t* url;    /* 下载地址（取第一个） */
    wchar_t* name;   /* basename */
} mp_file;
typedef struct {
    wchar_t* mcver;
    wchar_t* fabric;
    wchar_t* quilt;
    wchar_t* forge;
    wchar_t* neoforge;
    mp_file* files;
    int nfiles;
} mp_index;

static void mp_index_free(mp_index* x) {
    free(x->mcver); free(x->fabric); free(x->quilt); free(x->forge); free(x->neoforge);
    for (int i = 0; i < x->nfiles; i++) {
        free(x->files[i].path); free(x->files[i].url); free(x->files[i].name);
    }
    free(x->files);
    memset(x, 0, sizeof(*x));
}

/* ---------------- 解析 modrinth.index.json ---------------- */
static int mp_parse_index(const wchar_t* path, mp_index* out, wchar_t* err, size_t errlen) {
    char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
    if (!json_load_file(path, &js, &t, &nt) || nt < 1) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"整合包索引格式错误");
        free(js); free(t); return 0;
    }
    memset(out, 0, sizeof(*out));
    const jsmntok_t* dep = jm(js, t, "dependencies");
    if (dep) {
        const jsmntok_t* k;
        if ((k = jm(js, dep, "minecraft"))) out->mcver = jwstrdup(js, k);
        if ((k = jm(js, dep, "fabric-loader"))) out->fabric = jwstrdup(js, k);
        if ((k = jm(js, dep, "quilt-loader"))) out->quilt = jwstrdup(js, k);
        if ((k = jm(js, dep, "forge"))) out->forge = jwstrdup(js, k);
        if ((k = jm(js, dep, "neoforge"))) out->neoforge = jwstrdup(js, k);
    }
    const jsmntok_t* files = jm(js, t, "files");
    mp_file* arr = NULL; int n = 0;
    if (files && files->type == JSMN_ARRAY) {
        const jsmntok_t* p = files + 1;
        for (int i = 0; i < files->size; i++) {
            const jsmntok_t* f = p;
            p = tok_skip(f);
            if (f->type != JSMN_OBJECT) continue;
            /* env.client == "unsupported" 跳过 */
            const jsmntok_t* env = jm(js, f, "env");
            if (env) {
                const jsmntok_t* cl = jm(js, env, "client");
                if (cl) {
                    char* cv = jstrdup(js, cl);
                    int skip = cv && strcmp(cv, "unsupported") == 0;
                    free(cv);
                    if (skip) continue;
                }
            }
            const jsmntok_t* dls = jm(js, f, "downloads");
            if (!dls || dls->type != JSMN_ARRAY || dls->size < 1) continue;
            const jsmntok_t* u0 = dls + 1;
            if (u0->type != JSMN_STRING) continue;
            const jsmntok_t* pt = jm(js, f, "path");
            if (!pt) continue;
            mp_file mf;
            mf.path = jwstrdup(js, pt);
            mf.url = jwstrdup(js, u0);
            mf.name = wdup(path_base(mf.path));
            arr = (mp_file*)xrealloc(arr, (n + 1) * sizeof(mp_file));
            arr[n++] = mf;
        }
    }
    out->files = arr; out->nfiles = n;
    free(js); free(t);
    if (!out->mcver) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"整合包索引缺少 Minecraft 版本依赖");
        return 0;
    }
    return 1;
}

/* ---------------- 递归删除目录（清理临时文件） ---------------- */
static void delete_tree(const wchar_t* p) {
    wchar_t pat[4096];
    wchar_t* sub = wcat3(p, L"\\*", L"");
    wcscpy_s(pat, 4096, sub);
    free(sub);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t* full = path_join(p, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                delete_tree(full);
                RemoveDirectoryW(full);
            } else {
                SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full);
            }
            free(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

/* 确保文件父目录存在 */
static void ensure_parent(const wchar_t* file) {
    wchar_t* copy = wdup(file);
    wchar_t* slash = wcsrchr(copy, L'\\');
    wchar_t* fwd = wcsrchr(copy, L'/');
    if (fwd && (!slash || fwd > slash)) slash = fwd;
    if (slash) { *slash = 0; ensure_dir_rec(copy); }
    free(copy);
}

/* .mrpack 的 path 来自网络，必须拒绝绝对路径 / 盘符 / ".." 穿越。 */
static int safe_rel_path(const wchar_t* p) {
    if (!p || !*p) return 0;
    if (p[0] == L'\\' || p[0] == L'/') return 0;
    if (p[1] == L':') return 0;
    const wchar_t* s = p;
    while (*s) {
        const wchar_t* e = s;
        while (*e && *e != L'\\' && *e != L'/') e++;
        size_t n = (size_t)(e - s);
        if (n == 2 && s[0] == L'.' && s[1] == L'.') return 0;
        s = (*e) ? e + 1 : e;
    }
    return 1;
}

/* 版本号必须作为完整段出现：1.21.1 不能匹配 1.21.11。 */
static int has_ver_segment(const wchar_t* id, const wchar_t* v) {
    if (!v || !*v) return 1;
    size_t n = wcslen(v);
    const wchar_t* p = id;
    while ((p = wcsstr(p, v)) != NULL) {
        int left = (p == id) || (p[-1] == L'-');
        int right = (p[n] == 0) || (p[n] == L'-');
        if (left && right) return 1;
        p += n;
    }
    return 0;
}

static wchar_t* find_loader_instance(const wchar_t* gamedir, const wchar_t* mcver,
                                     const wchar_t* loader_ver) {
    wchar_t* vroot = path_join(gamedir, L"versions");
    wchar_t* found = NULL;
    uint64_t best = 0;
    wchar_t* pat = path_join(vroot, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == L'.') continue;
            const wchar_t* id = fd.cFileName;
            int is_loader = wcsstr(id, L"fabric") || wcsstr(id, L"quilt") ||
                            wcsstr(id, L"forge") || wcsstr(id, L"neoforge");
            if (!is_loader) continue;
            if (loader_ver && *loader_ver && !has_ver_segment(id, loader_ver)) continue;
            /* 不能只看目录名是否含 mcver：NeoForge 的目录可能是
               neoforge-21.1.265（不含 1.21.1），必须以 JSON 的 inheritsFrom 为准。 */
            int inherits_ok = 0;
            {
                wchar_t* vj = path_join(vroot, id);
                wchar_t* jn = wcat3(id, L".json", L"");
                wchar_t* jp = path_join(vj, jn);
                free(jn);
                char* js = NULL; jsmntok_t* t = NULL; int nt = 0;
                if (json_load_file(jp, &js, &t, &nt) && js) {
                    const jsmntok_t* ih = jm(js, t, "inheritsFrom");
                    if (ih && ih->type == JSMN_STRING) {
                        wchar_t* par = jwstrdup(js, ih);
                        if (par && weq(par, mcver)) inherits_ok = 1;
                        free(par);
                    }
                    json_free(js, t);
                }
                free(vj); free(jp);
            }
            if (!inherits_ok) {
                /* 兼容部分加载器把原版版本写进 id 但不写 inheritsFrom 的情况 */
                if (!has_ver_segment(id, mcver)) continue;
            }
            ULARGE_INTEGER mt;
            mt.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            mt.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            if (mt.QuadPart >= best) {
                best = mt.QuadPart;
                free(found);
                found = wdup(id);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(pat);
    free(vroot);
    if (!found) return NULL;
    wchar_t* a = path_join3(gamedir, L"versions", found);
    wchar_t* b = path_join(a, L"instance");
    free(a); free(found);
    return b;
}

/* ---------------- 主入口 ---------------- */
int modpack_install(const wchar_t* gamedir, const wchar_t* mrpack_path, int isolate,
                    dl_progress* prog, wchar_t* err, size_t errlen) {
    wchar_t* gd = wdup(gamedir ? gamedir : L".");
    wchar_t* tmp = path_join3(gd, L".llcl_tmp", L"mp");
    delete_tree(tmp);
    ensure_dir_rec(tmp);

    /* 1. 解压 mrpack 全部到临时目录 */
    int cnt = 0;
    if (!zip_extract_prefix(mrpack_path, NULL, tmp, &cnt)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"无法读取整合包文件（可能损坏或不是 .mrpack）");
        delete_tree(tmp);
        free(tmp); free(gd);
        return 0;
    }
    /* 2. 解析索引 */
    wchar_t* idxpath = path_join(tmp, L"modrinth.index.json");
    mp_index idx;
    if (!file_exists(idxpath) || !mp_parse_index(idxpath, &idx, err, errlen)) {
        if (!file_exists(idxpath))
            _snwprintf_s(err, errlen, _TRUNCATE, L"整合包内缺少 modrinth.index.json");
        free(idxpath); delete_tree(tmp);
        free(tmp); free(gd);
        return 0;
    }
    free(idxpath);

    /* 3. 安装游戏版本 */
    if (!dl_install_version(gd, idx.mcver, NULL, 0, prog, err, errlen)) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"安装游戏版本 %s 失败", idx.mcver);
        mp_index_free(&idx); delete_tree(tmp);
        free(tmp); free(gd);
        return 0;
    }
    /* 4. 安装加载器（Fabric/Quilt 走 meta；Forge/NeoForge 运行 installer） */
    ld_kind lkind = LD_FABRIC;
    const wchar_t* loader = NULL;
    if (idx.fabric)       { lkind = LD_FABRIC;    loader = idx.fabric; }
    else if (idx.quilt)   { lkind = LD_QUILT;     loader = idx.quilt; }
    else if (idx.neoforge){ lkind = LD_NEOFORGE;  loader = idx.neoforge; }
    else if (idx.forge)   { lkind = LD_FORGE;     loader = idx.forge; }
    if (loader) {
        /* 整合包为原版版本，dir_id = mcver */
        if (!loader_install(lkind, gd, idx.mcver, idx.mcver, loader, prog, err, errlen)) {
            mp_index_free(&idx); delete_tree(tmp);
            free(tmp); free(gd);
            return 0;
        }
    }

    /* 5. 确定内容安装根目录。
       开启版本隔离时，Mod/配置必须放进该加载器版本的 instance 目录，
       否则游戏从 instance 启动时根本看不到这些文件（旧实现总是写共享目录）。 */
    wchar_t* content_base = wdup(gd);
    if (isolate) {
        wchar_t* inst = find_loader_instance(gd, idx.mcver, loader);
        if (!inst) {
            /* 无加载器的整合包：退回原版版本的 instance */
            wchar_t* a = path_join3(gd, L"versions", idx.mcver);
            inst = path_join(a, L"instance");
            free(a);
        }
        if (inst && ensure_dir_rec(inst)) {
            free(content_base);
            content_base = inst;
        } else {
            free(inst);
        }
    }

    /* 6. 并发下载整合包文件 */
    dl_job* jobs = NULL; size_t njob = 0;
    int unsafe = 0;
    for (int i = 0; i < idx.nfiles; i++) {
        if (!safe_rel_path(idx.files[i].path)) { unsafe = 1; continue; }
        wchar_t* fp = path_join(content_base, idx.files[i].path);
        ensure_parent(fp);
        if (!file_exists(fp)) {
            jobs = (dl_job*)xrealloc(jobs, (njob + 1) * sizeof(dl_job));
            jobs[njob].url = wdup(idx.files[i].url);
            jobs[njob].dest = fp;
            jobs[njob].name = wdup(idx.files[i].name);
            jobs[njob].size = 0;
            njob++;
        } else {
            free(fp);
        }
    }
    int ok = !unsafe;
    if (unsafe) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"整合包索引包含非法路径（绝对路径或 ..），已拒绝安装");
    } else {
        ok = dl_run_jobs(jobs, njob, prog, err, errlen);
    }
    for (size_t i = 0; i < njob; i++) { free(jobs[i].url); free(jobs[i].dest); free(jobs[i].name); }
    free(jobs);

    /* 7. 解压 overrides（含客户端专用覆盖） */
    if (ok) {
        int oc = 0;
        zip_extract_prefix(mrpack_path, "overrides/", content_base, &oc);
        zip_extract_prefix(mrpack_path, "client-overrides/", content_base, &oc);
    }
    free(content_base);

    /* 7. 清理临时目录（mp 子目录 + .llcl_tmp 父目录） */
    mp_index_free(&idx);
    delete_tree(tmp);
    RemoveDirectoryW(tmp);
    wchar_t* tmpParent = path_join(gd, L".llcl_tmp");
    RemoveDirectoryW(tmpParent);
    free(tmpParent);
    free(tmp); free(gd);

    if (!ok) {
        _snwprintf_s(err, errlen, _TRUNCATE, L"部分文件下载失败，可重新安装（已下载的会跳过）");
        return 0;
    }
    return 1;
}
