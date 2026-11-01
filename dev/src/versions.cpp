/* LLcl - versions.c : scan versions/, parse version.json (jsmn), rules filter,
 * inheritsFrom recursive merge */
#define JSMN_STATIC
#include "jsmn.h"
#include "llcl.h"

/* ---------------- scan ---------------- */
int scan_versions(const wchar_t* gamedir, ventry** out, int* n) {
    *out = NULL; *n = 0;
    wchar_t* vdir = path_join(gamedir, L"versions");
    if (!dir_exists(vdir)) { free(vdir); return 0; }
    wchar_t pattern[4096];
    _snwprintf_s(pattern, 4096, _TRUNCATE, L"%s\\*", vdir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { free(vdir); return 0; }
    ventry* list = NULL;
    int cnt = 0, cap = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* versions/<id>/<id>.json */
            wchar_t* sub = path_join(vdir, fd.cFileName);
            wchar_t* jp = path_join(sub, wcat3(fd.cFileName, L".json", L""));
            if (file_exists(jp)) {
                if (cnt == cap) { cap = cap ? cap * 2 : 16; list = (ventry*)xrealloc(list, cap * sizeof(ventry)); }
                list[cnt].id = wdup(fd.cFileName);
                list[cnt].json_path = jp;
                cnt++;
            } else {
                free(jp);
            }
            free(sub);
        } else {
            /* versions/<id>.json (legacy layout) */
            size_t l = wcslen(fd.cFileName);
            if (l > 5 && _wcsicmp(fd.cFileName + l - 5, L".json") == 0) {
                wchar_t* id = wdup(fd.cFileName);
                id[l - 5] = 0;
                /* 过滤官方清单文件（jre_manifest.json / version_manifest_v2.json 等），
                   它们不是游戏版本，误扫进列表会导致启动失败 */
                int skip = 0;
                for (const wchar_t* p = id; *p && !skip; p++)
                    if ((*p == L'm' || *p == L'M') && _wcsnicmp(p, L"manifest", 8) == 0) skip = 1;
                if (skip) {
                    free(id);
                } else {
                    wchar_t* jp = path_join(vdir, fd.cFileName);
                    if (cnt == cap) { cap = cap ? cap * 2 : 16; list = (ventry*)xrealloc(list, cap * sizeof(ventry)); }
                    list[cnt].id = id;
                    list[cnt].json_path = jp;
                    cnt++;
                }
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    free(vdir);
    if (cnt > 1) {
        for (int i = 1; i < cnt; i++) {
            ventry t = list[i];
            int j = i - 1;
            while (j >= 0 && _wcsicmp(list[j].id, t.id) > 0) { list[j + 1] = list[j]; j--; }
            list[j + 1] = t;
        }
    }
    *out = list;
    *n = cnt;
    return 1;
}

/* ---------------- rule evaluation (vanilla semantics) ---------------- */
static int os_arch_matches(const char* js, const struct jsmntok* arch) {
    int len = arch->end - arch->start;
    /* 本程序以 x64 构建：x86 系统走 32 位分支；arm64 原生库不适用。
     * 旧实现把 arm64 也算作匹配，会在 ARM64 Windows 上同时选中
     * x64 与 arm64 两套 natives，解压时互相覆盖。 */
    if (is_os64()) {
        if (len == 5 && _strnicmp(js + arch->start, "amd64", 5) == 0) return 1;
        if (len == 3 && _strnicmp(js + arch->start, "x64", 3) == 0) return 1;
        return 0;
    }
    if (len == 3 && _strnicmp(js + arch->start, "x86", 3) == 0) return 1;
    if (len == 4 && _strnicmp(js + arch->start, "i386", 4) == 0) return 1;
    return 0;
}

/* Parse up to 3 numeric components of an OS version string inside JSON.
 * Supports "10.0.17134", "6.1", "10.*". Returns component count. */
static int parse_ver_tok(const char* js, const struct jsmntok* t, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    if (!t || t->type != JSMN_STRING) return 0;
    int i = t->start, n = 0;
    while (i < t->end && n < 3) {
        while (i < t->end && (js[i] < '0' || js[i] > '9') && js[i] != '*') i++;
        if (i >= t->end) break;
        if (js[i] == '*') { v[n++] = -1; i++; continue; }
        int val = 0;
        while (i < t->end && js[i] >= '0' && js[i] <= '9') { val = val * 10 + (js[i] - '0'); i++; }
        v[n++] = val;
    }
    return n;
}

static int cmp_ver3(const int a[3], const int b[3], int n) {
    for (int i = 0; i < n; i++) {
        if (b[i] < 0) continue;          /* wildcard */
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

static int os_ver_matches(const char* js, const struct jsmntok* ver) {
    int cur[3] = { 0, 0, 0 };
    os_ver_numbers(&cur[0], &cur[1], &cur[2]);
    if (ver->type == JSMN_STRING) {
        int want[3];
        int n = parse_ver_tok(js, ver, want);
        if (n <= 0) return 1;             /* unknown pattern: permissive */
        /* Compare only the components the rule specifies (e.g. "10.0"). */
        return cmp_ver3(cur, want, n) == 0;
    }
    if (ver->type == JSMN_OBJECT) {
        const struct jsmntok* mn = jm(js, ver, "min");
        const struct jsmntok* mx = jm(js, ver, "max");
        if (mn) {
            int want[3];
            int n = parse_ver_tok(js, mn, want);
            if (n > 0 && cmp_ver3(cur, want, n) < 0) return 0;
        }
        if (mx) {
            int want[3];
            int n = parse_ver_tok(js, mx, want);
            if (n > 0 && cmp_ver3(cur, want, n) >= 0) return 0;
        }
        return 1;
    }
    return 1;
}

static int rule_matches_env(const char* js, const struct jsmntok* rule) {
    if (rule->type != JSMN_OBJECT) return 1;
    const struct jsmntok* os = jm(js, rule, "os");
    if (os) {
        const struct jsmntok* nm = jm(js, os, "name");
        if (nm && nm->type == JSMN_STRING) {
            int len = nm->end - nm->start;
            if (!(len == 7 && _strnicmp(js + nm->start, "windows", 7) == 0)) return 0;
        }
        const struct jsmntok* arch = jm(js, os, "arch");
        if (arch && arch->type == JSMN_STRING) {
            if (!os_arch_matches(js, arch)) return 0;
        }
        const struct jsmntok* ver = jm(js, os, "version");
        if (ver) {
            if (!os_ver_matches(js, ver)) return 0;
        }
        /* 26.x 新写法：os.versionRange = {min,max} */
        const struct jsmntok* vrange = jm(js, os, "versionRange");
        if (vrange) {
            if (!os_ver_matches(js, vrange)) return 0;
        }
    }
    const struct jsmntok* feats = jm(js, rule, "features");
    if (feats && feats->type == JSMN_OBJECT) {
        /* we never enable launcher features (is_demo_user etc.) */
        const struct jsmntok* p = feats + 1;
        for (int i = 0; i < feats->size; i++) {
            const struct jsmntok* k = p;
            const struct jsmntok* v = k + 1;
            p = tok_skip(k);
            p = tok_skip(v);
            if (v->type == JSMN_PRIMITIVE && (v->end - v->start) == 4 &&
                _strnicmp(js + v->start, "true", 4) == 0)
                return 0;
        }
    }
    return 1;
}

/* returns 1 = include */
static int eval_rules(const char* js, const struct jsmntok* rules) {
    if (!rules) return 1;
    if (rules->type != JSMN_ARRAY) return 1;
    const struct jsmntok* p = rules + 1;
    for (int i = 0; i < rules->size; i++) {
        const struct jsmntok* rule = p;
        p = tok_skip(rule);
        int allow = 1;
        const struct jsmntok* act = jm(js, rule, "action");
        if (act && act->type == JSMN_STRING) {
            int len = act->end - act->start;
            allow = (len == 5 && _strnicmp(js + act->start, "allow", 5) == 0) ? 1 : 0;
        }
        if (rule_matches_env(js, rule)) return allow;
    }
    return 0; /* no rule matched -> disallow (vanilla) */
}

/* ---------------- libraries ---------------- */
static void libname_parts(wchar_t* name, wchar_t** group, wchar_t** artifact, wchar_t** ver, wchar_t** cls) {
    /* g:a:v[:c] — split on ':' (4 max)；调用方必须传可变副本 */
    wchar_t* p = name;
    wchar_t* cols[4] = { NULL, NULL, NULL, NULL };
    int nc = 0;
    cols[nc++] = p;
    wchar_t* s = p;
    while (*s && nc < 4) {
        if (*s == L':') { *s = 0; cols[nc++] = s + 1; }
        s++;
    }
    *group = (wchar_t*)cols[0];
    *artifact = (wchar_t*)cols[1];
    *ver = (wchar_t*)cols[2];
    *cls = (nc >= 4) ? (wchar_t*)cols[3] : NULL;
}

static wchar_t* lib_rel_from_name(const wchar_t* name) {
    wchar_t* nm = wdup(name);
    wchar_t *g, *a, *v, *c;
    libname_parts(nm, &g, &a, &v, &c);
    if (!g || !a || !v) { free(nm); return NULL; }
    wsb b;
    wsb_init(&b);
    for (const wchar_t* q = g; *q; q++) wsb_addc(&b, *q == L'.' ? L'/' : *q);
    wsb_addf(&b, L"/%s/%s/%s-%s", a, v, a, v);
    if (c) wsb_addf(&b, L"-%s", c);
    wsb_add(&b, L".jar");
    wchar_t* r = wsb_take(&b);
    wsb_free(&b);
    free(nm);
    return r;
}

static int parse_libs(const wchar_t* gamedir, const char* js, const struct jsmntok* libs_tok, vinfo* v) {
    const struct jsmntok* p = libs_tok + 1;
    for (int i = 0; i < libs_tok->size; i++) {
        const struct jsmntok* lib = p;
        p = tok_skip(lib);
        if (lib->type != JSMN_OBJECT) continue;
        const struct jsmntok* rules = jm(js, lib, "rules");
        if (!eval_rules(js, rules)) continue;
        const struct jsmntok* ntok = jm(js, lib, "name");
        if (!ntok || ntok->type != JSMN_STRING) continue;
        wchar_t* name = jwstrdup(js, ntok);
        if (!name) continue;
        /* classifier selection */
        wchar_t* cls = NULL;
        const struct jsmntok* nat = jm(js, lib, "natives");
        if (nat && nat->type == JSMN_OBJECT) {
            const struct jsmntok* w = jm(js, nat, "windows");
            if (w) cls = jwstrdup(js, w);
        } else {
            /* new style: name has ":natives-..." classifier */
            wchar_t* nm2 = wdup(name);
            wchar_t *g, *a, *v, *c;
            libname_parts(nm2, &g, &a, &v, &c);
            if (c && wstartswith(c, L"natives-")) cls = wdup(c);
            free(nm2);
        }
        /* 1.8.x 老版本 natives 规则含 ${arch} 占位符 → 本启动器固定 64 位 */
        if (cls) {
            wchar_t* p = wcsstr(cls, L"${arch}");
            while (p) {
                size_t n = wcslen(cls);
                memmove(p + 2, p + 7, (n - (size_t)(p - cls) - 6) * sizeof(wchar_t));
                p[0] = L'6'; p[1] = L'4';
                p = wcsstr(p + 2, L"${arch}");
            }
        }
        int is_native = cls != NULL;

        /* relative path: prefer downloads.*.path, else derive from name */
        wchar_t* rel = NULL;
        uint64_t lsize = 0;
        const struct jsmntok* dl = jm(js, lib, "downloads");
        if (dl) {
            const struct jsmntok* pt = NULL;
            const struct jsmntok* ent = NULL;
            if (cls) {
                const struct jsmntok* cfs = jm(js, dl, "classifiers");
                if (cfs) {
                    char* ck8 = w2u(cls);
                    const struct jsmntok* cj = ck8 ? jm(js, cfs, ck8) : NULL;
                    if (cj) { ent = cj; pt = jm(js, cj, "path"); }
                    free(ck8);
                }
            } else {
                const struct jsmntok* art = jm(js, dl, "artifact");
                if (art) { ent = art; pt = jm(js, art, "path"); }
            }
            if (pt && pt->type == JSMN_STRING) rel = jwstrdup(js, pt);
            if (ent) {
                const struct jsmntok* sz = jm(js, ent, "size");
                if (sz && sz->type == JSMN_PRIMITIVE) lsize = strtoull(js + sz->start, NULL, 10);
            }
        }
        free(cls);

        if (!rel) {
            if (is_native) {
                /* old style native: rebuild name with classifier */
                wchar_t* nm3 = wdup(name);
                wchar_t *g, *a, *v, *c;
                libname_parts(nm3, &g, &a, &v, &c);
                (void)c;
                wchar_t* natcls = NULL;
                const struct jsmntok* nat2 = jm(js, lib, "natives");
                if (nat2) {
                    const struct jsmntok* w2 = jm(js, nat2, "windows");
                    if (w2) natcls = jwstrdup(js, w2);
                }
                if (natcls) {
                    wsb b; wsb_init(&b);
                    for (const wchar_t* q = g; *q; q++) wsb_addc(&b, *q == L'.' ? L'/' : *q);
                    wsb_addf(&b, L"/%s/%s/%s-%s-%s.jar", a, v, a, v, natcls);
                    rel = wsb_take(&b); wsb_free(&b);
                    free(natcls);
                }
                free(nm3);
            }
        }
        if (!rel) rel = lib_rel_from_name(name);
        if (!rel) { free(name); continue; }
        if (is_native && !rel) { free(name); continue; }

        v->libs = (libitem*)xrealloc(v->libs, (v->nlibs + 1) * sizeof(libitem));
        libitem* li = &v->libs[v->nlibs];
        memset(li, 0, sizeof(*li));
        li->name = name;
        li->rel = rel;
        li->size = lsize;
        li->is_native = is_native;
        wchar_t* libroot = path_join(gamedir, L"libraries");
        li->fullpath = path_join(libroot, rel);
        free(libroot);
        li->present = file_exists(li->fullpath);
        if (li->present && li->size > 0) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW(li->fullpath, GetFileExInfoStandard, &fa)) {
                uint64_t sz = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                if (sz != li->size) li->present = 0;
            }
        }
        v->nlibs++;
    }
    return 1;
}

/* ---------------- arguments ---------------- */
static void parse_arg_array(const char* js, const struct jsmntok* arr, wsa* out) {
    const struct jsmntok* p = arr + 1;
    for (int i = 0; i < arr->size; i++) {
        const struct jsmntok* t = p;
        p = tok_skip(t);
        if (t->type == JSMN_STRING) {
            wchar_t* s = jwstrdup(js, t);
            if (s) wsa_addown(out, s);
        } else if (t->type == JSMN_OBJECT) {
            const struct jsmntok* rules = jm(js, t, "rules");
            if (!eval_rules(js, rules)) continue;
            const struct jsmntok* val = jm(js, t, "value");
            if (!val) continue;
            if (val->type == JSMN_STRING) {
                wchar_t* s = jwstrdup(js, val);
                if (s) wsa_addown(out, s);
            } else if (val->type == JSMN_ARRAY) {
                const struct jsmntok* q = val + 1;
                for (int j = 0; j < val->size; j++) {
                    const struct jsmntok* it = q;
                    q = tok_skip(it);
                    if (it->type == JSMN_STRING) {
                        wchar_t* s = jwstrdup(js, it);
                        if (s) wsa_addown(out, s);
                    }
                }
            }
        }
    }
}

/* ---------------- parse single json ---------------- */
static int parse_part(const wchar_t* gamedir, const wchar_t* json_path, vinfo* v) {
    char* js;
    struct jsmntok* toks;
    int nt;
    if (!json_load_file(json_path, &js, &toks, &nt)) return 0;
    struct jsmntok* root = toks;
    if (!root || root->type != JSMN_OBJECT) { json_free(js, toks); return 0; }
    wchar_t* tmp;
    if ((tmp = jwstrdup(js, jm(js, root, "id")))) v->id = tmp;
    if ((tmp = jwstrdup(js, jm(js, root, "inheritsFrom")))) v->inherits_from = tmp;
    if ((tmp = jwstrdup(js, jm(js, root, "mainClass")))) v->main_class = tmp;
    if ((tmp = jwstrdup(js, jm(js, root, "type")))) v->version_type = tmp;
    const struct jsmntok* ai = jm(js, root, "assetIndex");
    if (ai) {
        const struct jsmntok* aid = jm(js, ai, "id");
        if (aid) v->assets = jwstrdup(js, aid);
    } else {
        const struct jsmntok* a = jm(js, root, "assets");
        if (a) v->assets = jwstrdup(js, a);
    }
    const struct jsmntok* jv = jm(js, root, "javaVersion");
    if (jv) {
        const struct jsmntok* maj = jm(js, jv, "majorVersion");
        if (maj && maj->type == JSMN_PRIMITIVE) v->java_major = atoi(js + maj->start);
    }
    const struct jsmntok* args = jm(js, root, "arguments");
    if (args) {
        v->has_modern = 1;
        const struct jsmntok* jvm = jm(js, args, "jvm");
        if (jvm && jvm->type == JSMN_ARRAY) parse_arg_array(js, jvm, &v->jvm);
        const struct jsmntok* game = jm(js, args, "game");
        if (game && game->type == JSMN_ARRAY) parse_arg_array(js, game, &v->game);
    }
    const struct jsmntok* ma = jm(js, root, "minecraftArguments");
    if (ma && ma->type == JSMN_STRING) {
        v->legacy = jwstrdup(js, ma);
        if (v->legacy) v->has_legacy = 1;
    }
    const struct jsmntok* libs = jm(js, root, "libraries");
    if (libs && libs->type == JSMN_ARRAY) parse_libs(gamedir, js, libs, v);
    json_free(js, toks);
    return 1;
}

/* ---------------- merge (base=parent, over=child) ---------------- */
static void vinfo_combine(const vinfo* base, const vinfo* over, vinfo* out) {
    memset(out, 0, sizeof(*out));
    out->id = wdup(over->id ? over->id : base->id);
    /* 游戏本体 jar 取继承链根版本（Fabric/Quilt 自身目录无 jar） */
    out->jar_id = wdup(base->jar_id ? base->jar_id : base->id);
    out->main_class = wdup(over->main_class ? over->main_class : base->main_class);
    out->assets = wdup(over->assets ? over->assets : base->assets);
    out->version_type = wdup(over->version_type ? over->version_type : (base->version_type ? base->version_type : L"release"));
    out->java_major = over->java_major ? over->java_major : base->java_major;
    if (base->has_modern || over->has_modern) {
        out->has_modern = 1;
        /* jvm 参数：父版本在前（含 -cp、-Djava.library.path 等），子版本追加在后。
         * Forge/NeoForge 用 -p 模块路径，但仍需父版本的 -cp（BootstrapLauncher 从
         * java.class.path 扫描 modlauncher/fmlloader 等模块）；其 classpath 内容由
         * launch.cpp 特殊处理（不含原版 client jar，避免与运行时 minecraft 模块冲突）。 */
        for (size_t i = 0; i < base->jvm.n; i++) wsa_add(&out->jvm, base->jvm.v[i]);
        for (size_t i = 0; i < over->jvm.n; i++) wsa_add(&out->jvm, over->jvm.v[i]);
        /* game 参数：父版本在前，子版本追加在后 */
        for (size_t i = 0; i < base->game.n; i++) wsa_add(&out->game, base->game.v[i]);
        for (size_t i = 0; i < over->game.n; i++) wsa_add(&out->game, over->game.v[i]);
    }
    if (over->has_legacy) {
        out->has_legacy = 1;
        out->legacy = wdup(over->legacy);
    } else if (base->has_legacy) {
        out->has_legacy = 1;
        out->legacy = wdup(base->legacy);
    }
    /* child libs first (override), then parent libs not already present */
    for (size_t i = 0; i < over->nlibs; i++) {
        libitem* li = &over->libs[i];
        out->libs = (libitem*)xrealloc(out->libs, (out->nlibs + 1) * sizeof(libitem));
        memset(&out->libs[out->nlibs], 0, sizeof(libitem));
        out->libs[out->nlibs].name = wdup(li->name);
        out->libs[out->nlibs].rel = wdup(li->rel);
        out->libs[out->nlibs].fullpath = wdup(li->fullpath);
        out->libs[out->nlibs].size = li->size;
        out->libs[out->nlibs].is_native = li->is_native;
        out->libs[out->nlibs].present = li->present;
        out->nlibs++;
    }
    for (size_t i = 0; i < base->nlibs; i++) {
        const libitem* li = &base->libs[i];
        int dup = 0;
        for (size_t j = 0; j < out->nlibs; j++)
            if (weqi(out->libs[j].name, li->name)) { dup = 1; break; }
        if (dup) continue;
        out->libs = (libitem*)xrealloc(out->libs, (out->nlibs + 1) * sizeof(libitem));
        memset(&out->libs[out->nlibs], 0, sizeof(libitem));
        out->libs[out->nlibs].name = wdup(li->name);
        out->libs[out->nlibs].rel = wdup(li->rel);
        out->libs[out->nlibs].fullpath = wdup(li->fullpath);
        out->libs[out->nlibs].size = li->size;
        out->libs[out->nlibs].is_native = li->is_native;
        out->libs[out->nlibs].present = li->present;
        out->nlibs++;
    }
}

void vinfo_free(vinfo* v) {
    if (!v) return;
    free(v->id); free(v->inherits_from); free(v->jar_id); free(v->main_class); free(v->assets);
    free(v->version_type); free(v->legacy);
    wsa_free(&v->jvm);
    wsa_free(&v->game);
    for (size_t i = 0; i < v->nlibs; i++) {
        free(v->libs[i].name); free(v->libs[i].rel); free(v->libs[i].fullpath);
    }
    free(v->libs);
    memset(v, 0, sizeof(*v));
}

void vinfo_clone(vinfo* dst, const vinfo* src) {
    memset(dst, 0, sizeof(*dst));
    dst->id = wdup(src->id);
    dst->inherits_from = wdup(src->inherits_from);
    dst->jar_id = wdup(src->jar_id);
    dst->main_class = wdup(src->main_class);
    dst->assets = wdup(src->assets);
    dst->version_type = wdup(src->version_type);
    dst->java_major = src->java_major;
    dst->has_modern = src->has_modern;
    dst->has_legacy = src->has_legacy;
    for (size_t i = 0; i < src->jvm.n; i++) wsa_add(&dst->jvm, src->jvm.v[i]);
    for (size_t i = 0; i < src->game.n; i++) wsa_add(&dst->game, src->game.v[i]);
    dst->legacy = wdup(src->legacy);
    dst->nlibs = src->nlibs;
    if (src->nlibs) {
        dst->libs = (libitem*)xalloc(src->nlibs * sizeof(libitem));
        for (size_t i = 0; i < src->nlibs; i++) {
            memset(&dst->libs[i], 0, sizeof(libitem));
            dst->libs[i].name = wdup(src->libs[i].name);
            dst->libs[i].rel = wdup(src->libs[i].rel);
            dst->libs[i].fullpath = wdup(src->libs[i].fullpath);
            dst->libs[i].size = src->libs[i].size;
            dst->libs[i].is_native = src->libs[i].is_native;
            dst->libs[i].present = src->libs[i].present;
        }
    }
}

/* ---------------- resolve with inheritsFrom ---------------- */
static wchar_t* version_json_path(const wchar_t* gamedir, const wchar_t* id) {
    wchar_t* sub = path_join3(gamedir, L"versions", id);
    wchar_t* p1 = wcat3(sub, L"\\", id);
    wchar_t* p2 = wcat3(p1, L".json", L"");
    free(p1);
    if (file_exists(p2)) { free(sub); return p2; }
    free(p2);
    wchar_t* p3 = wcat3(sub, L".json", L"");
    free(sub);
    if (file_exists(p3)) return p3;
    free(p3);
    return NULL;
}

static vinfo* resolve_rec(const wchar_t* gamedir, const wchar_t* id, int depth, wsa* seen) {
    if (depth > 8) {
        ui_log(L"版本 %s: 继承链过深（>8），已中止", id);
        return NULL;
    }
    if (wsa_has(seen, id)) {
        ui_log(L"版本 %s: 检测到循环继承（inheritsFrom 环），已中止", id);
        return NULL;
    }
    wsa_add(seen, id);
    wchar_t* jp = version_json_path(gamedir, id);
    if (!jp) {
        ui_log(L"版本 %s: 找不到版本 JSON（versions/%s.json）", id, id);
        return NULL;
    }
    vinfo part;
    memset(&part, 0, sizeof(part));
    if (!parse_part(gamedir, jp, &part)) {
        ui_log(L"版本 %s: JSON 解析失败", id);
        free(jp);
        return NULL;
    }
    free(jp);
    vinfo* result = NULL;
    if (part.inherits_from) {
        wchar_t* parent_id = wdup(part.inherits_from);
        vinfo* base = resolve_rec(gamedir, parent_id, depth + 1, seen);
        free(parent_id);
        if (base) {
            result = (vinfo*)xalloc(sizeof(vinfo));
            vinfo_combine(base, &part, result);
            vinfo_free(base);
            free(base);
            vinfo_free(&part); /* combine 已复制全部数据 */
        } else {
            ui_log(L"版本 %s: 父版本 %s 解析失败，仅使用自身数据", id, part.inherits_from);
            result = (vinfo*)xalloc(sizeof(vinfo));
            *result = part;
            memset(&part, 0, sizeof(part)); /* 所有权已移交，不得释放 */
            result->jar_id = wdup(result->id);
        }
    } else {
        result = (vinfo*)xalloc(sizeof(vinfo));
        *result = part;
        memset(&part, 0, sizeof(part));     /* 所有权已移交，不得释放 */
        result->jar_id = wdup(result->id);  /* 根版本：游戏 jar 即自身 */
    }
    return result;
}

int resolve_version(const wchar_t* gamedir, const wchar_t* id, vinfo* out) {
    memset(out, 0, sizeof(*out));
    wsa seen;
    memset(&seen, 0, sizeof(seen));
    vinfo* r = resolve_rec(gamedir, id, 0, &seen);
    wsa_free(&seen);
    if (!r) return 0;
    *out = *r;
    free(r);
    if (!out->main_class) out->main_class = wdup(L"net.minecraft.client.main.Main");
    if (!out->assets) out->assets = wdup(L"legacy");
    if (!out->version_type) out->version_type = wdup(L"release");
    return 1;
}
