/* LLcl - Minecraft Java launcher (Win32, C11, /MT static)
 * llcl.h - shared declarations & small helpers
 */
#ifndef LLCL_H
#define LLCL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define LLCL_VERSION L"1.4.0"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <wininet.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

/* ---------------- util ---------------- */
wchar_t* u2w(const char* s);              /* utf8 -> utf16 (malloc) */
char* w2u(const wchar_t* s);              /* utf16 -> utf8 (malloc) */
wchar_t* wdup(const wchar_t* s);
char* udup(const char* s);
void* xalloc(size_t n);
void* xrealloc(void* p, size_t n);
wchar_t* wcat3(const wchar_t* a, const wchar_t* b, const wchar_t* c);
int weq(const wchar_t* a, const wchar_t* b);
int weqi(const wchar_t* a, const wchar_t* b);
int wstartswith(const wchar_t* s, const wchar_t* pre);

typedef struct { wchar_t* b; size_t n, cap; } wsb;
void wsb_init(wsb* b);
void wsb_free(wsb* b);
void wsb_add(wsb* b, const wchar_t* s);
void wsb_addc(wsb* b, wchar_t c);
void wsb_addf(wsb* b, const wchar_t* fmt, ...);
wchar_t* wsb_take(wsb* b);                /* transfer buffer out (caller frees) */

typedef struct { wchar_t** v; size_t n, cap; } wsa;
void wsa_add(wsa* a, const wchar_t* s);
void wsa_addown(wsa* a, wchar_t* s);      /* takes ownership */
int  wsa_has(wsa* a, const wchar_t* s);   /* case-insensitive */
void wsa_free(wsa* a);

uint64_t now_us(void);
void get_stamp(wchar_t* out, size_t n);   /* "HH:mm:ss" */
int file_exists(const wchar_t* p);
int dir_exists(const wchar_t* p);
int is_os64(void);
int os_ver_major(void);
void os_ver_numbers(int* major, int* minor, int* build);
int zip_extract_prefix(const wchar_t* zip_path, const char* prefix, const wchar_t* dest_dir, int* out_count);
int zip_extract_dlls(const wchar_t* zip_path, const wchar_t* dest_dir, int* out_count);
unsigned char* zip_read_file(const wchar_t* zip_path, const char* target, size_t* out_len);
unsigned char* zip_inflate_raw(const unsigned char* in, size_t in_len, size_t out_cap, size_t* out_len);
wchar_t* path_join(const wchar_t* a, const wchar_t* b);  /* a\b */
wchar_t* path_join3(const wchar_t* a, const wchar_t* b, const wchar_t* c);
const wchar_t* path_base(const wchar_t* p);
void strip_slash(wchar_t* p);
int ensure_dir(const wchar_t* p);         /* create one level; 1 ok/exists */
int ensure_dir_rec(const wchar_t* p);     /* create full path; 1 ok */
wchar_t* get_exe_dir(void);
wchar_t* abs_path(const wchar_t* p);

/* ---------------- json (jsmn wrapper; struct jsmntok is opaque) ---------------- */
struct jsmntok;
int json_load_file(const wchar_t* path, char** js, struct jsmntok** toks, int* ntoks);
int json_parse_mem(const char* js, size_t len, struct jsmntok** toks, int* ntoks);
void json_free(char* js, struct jsmntok* toks);
const struct jsmntok* tok_skip(const struct jsmntok* t);
const struct jsmntok* jm(const char* js, const struct jsmntok* obj, const char* key);
char* jstrdup(const char* js, const struct jsmntok* t);       /* utf8, unescaped */
wchar_t* jwstrdup(const char* js, const struct jsmntok* t);
int jbool(const char* js, const struct jsmntok* t);

/* ---------------- versions ---------------- */
typedef struct { wchar_t* id; wchar_t* json_path; } ventry;
typedef struct {
    wchar_t* name;       /* g:a:v[:c] */
    wchar_t* rel;        /* relative path under libraries\ */
    wchar_t* fullpath;   /* gamedir\libraries\rel */
    uint64_t size;       /* expected file size from version JSON (0 = unknown) */
    int is_native;
    int present;
} libitem;
typedef struct {
    wchar_t* id;
    wchar_t* inherits_from;
    wchar_t* jar_id;        /* 游戏本体 jar 所在版本（继承链根，通常是原版版本），用于 classpath */
    wchar_t* main_class;
    wchar_t* assets;         /* assetIndex id (or legacy "assets") */
    wchar_t* version_type;
    int  java_major;
    int  has_modern;         /* "arguments" (jvm+game) */
    int  has_legacy;         /* "minecraftArguments" */
    wsa  jvm;                /* modern jvm args, raw */
    wsa  game;               /* modern game args, raw */
    wchar_t* legacy;         /* minecraftArguments template */
    libitem* libs;
    size_t nlibs;
} vinfo;
void vinfo_free(vinfo* v);
void vinfo_clone(vinfo* dst, const vinfo* src);
int scan_versions(const wchar_t* gamedir, ventry** out, int* n);
int resolve_version(const wchar_t* gamedir, const wchar_t* id, vinfo* out);

/* ---------------- java ---------------- */
typedef struct { wchar_t* path; wchar_t* fullver; int major; int is64; } javaitem;
void javas_free(javaitem* a, int n);
int detect_javas(javaitem** out, int* n);                     /* verified+sorted */
int java_verify(const wchar_t* java_exe, wchar_t* fullver, size_t cap, int* major, int* is64);
const wchar_t* java_major_req_name(int major);                /* "8"/"17"/"21"/"?" */

/* ---------------- memory ---------------- */
uint64_t total_phys_bytes(void);
int auto_heap_mb(void);
int auto_xms_mb(int heap);
void mem_trim(void);

/* ---------------- config ---------------- */
typedef struct {
    wchar_t* java_path;
    int      mem_mb;        /* 0 = auto */
    wchar_t* player;
    wchar_t* game_dir;
    wchar_t* last_version;
    int      version_isolate;  /* 1 = 每个版本独立存档/模组/配置 */
    int      mem_opt;          /* 1 = 运行内存优化（GC 参数 + 启动前释放启动器内存） */
    int      dl_threads;       /* 下载并发线程数（8 或 16，默认 8） */
    wchar_t* msa_refresh;      /* 微软 refresh_token（正版登录） */
    wchar_t* msa_access;       /* 微软 access_token（手动 token / 最近一次刷新） */
    wchar_t* msa_name;         /* 微软账号名 */
    wchar_t* msa_uuid;         /* 正版 UUID */
    wchar_t* msa_xuid;         /* 正版 XUID（1.21+ 的 --xuid 参数） */
    wchar_t* msa_client;       /* 自定义 Azure client_id（空 = 内置公共） */
    wchar_t* relay_server;     /* 远程联机中转服务器 host:port（空 = 未配置） */
    int      net_mode;         /* 0 = 直连，1 = 中转 */
    int      net_port;         /* 直连房间端口，0 = 自动 */
    wchar_t* net_invite;       /* 上一次的邀请码（方便再次分享） */
} cfg;
void cfg_init(cfg* c);
void cfg_free(cfg* c);
int cfg_load(const wchar_t* path, cfg* c);
int cfg_save(const wchar_t* path, const cfg* c);

/* ---------------- launch ---------------- */
typedef struct {
    uint64_t us_natives, us_build, us_spawn, us_total;
    DWORD pid;
    int ok;
    int missing_libs;
    int early_exit;          /* 子进程在观察窗口内退出（秒退/崩溃），视为启动异常 */
    DWORD exit_code;         /* 子进程退出码（early_exit 时有效） */
    wchar_t log_path[512];   /* java stdout/stderr 重定向日志文件 */
    wchar_t err[640];
} lresult;
void lresult_init(lresult* r);
int launch_game(const vinfo* v, const wchar_t* gamedir, const wchar_t* java_exe,
                int mem_mb, const wchar_t* player, const wchar_t* uuid,
                const wchar_t* access_token, const wchar_t* xuid, int isolate,
                int mem_opt, lresult* r);
int extract_zip(const wchar_t* zip, const wchar_t* dest);   /* COM zipfldr */

/* 让下一次 launch_game 带上 --server/--port（连接多人服务器用） */
void launch_set_server(const wchar_t* addr);

/* ---------------- download ---------------- */
typedef struct {
    volatile LONG64 bytes_done;   /* 已下载字节（跨线程累加，含进行中文件的当前进度） */
    volatile LONG   files_done;   /* 已完成文件数 */
    volatile LONG   files_total;  /* 待下载文件总数 */
    volatile LONG   active;       /* 正在下载的工作线程数 */
    volatile LONG   failed;       /* 失败文件数 */
    wchar_t cur[256];             /* 当前正在下载的文件名（仅供显示，允许竞争写） */
} dl_progress;
typedef struct { wchar_t* id; wchar_t* url; } dl_ver;
typedef struct { dl_ver* v; int n, cap; } dl_list;
int dl_install_version(const wchar_t* gamedir, const wchar_t* version_id,
                       const wchar_t* custom_name, int skip_assets, dl_progress* prog,
                       wchar_t* err, size_t errlen);
int dl_fetch_manifest(dl_list* out, wchar_t* err, size_t errlen);
void dl_list_free(dl_list* l);
typedef struct { wchar_t* url; wchar_t* dest; wchar_t* name; uint64_t size; } dl_job;
extern int g_dl_threads;   /* 下载并发数（8/16），由设置界面同步，download.c 定义 */
int dl_run_jobs(dl_job* jobs, size_t n, dl_progress* prog, wchar_t* err, size_t errlen);  /* 并发下载一组 */

/* ---------------- net (shared WinINet helpers, defined in download.c) ---------------- */
int  net_init(HINTERNET* inet);                                   /* 建 session + 超时/并发设置 */
int  net_get(HINTERNET inet, const wchar_t* url,
             unsigned char** out, size_t* out_len, int* status);  /* GET 全读到内存（API JSON） */
int  net_download_file(HINTERNET inet, const wchar_t* url, const wchar_t* dest,
                       void(*cb)(uint64_t cur, uint64_t total, void* ctx), void* ctx);  /* GET 流式写文件 */
void net_urlenc(wsb* b, const wchar_t* s);   /* URL 编码后追加（UTF-8 百分号编码） */

/* ---------------- content (Modrinth: Mod/光影/资源包) ---------------- */
typedef enum { CT_MOD = 0, CT_SHADER = 1, CT_RP = 2, CT_MODPACK = 3 } ct_type;
typedef struct {
    wchar_t* id;
    wchar_t* title;
    wchar_t* author;
    wchar_t* desc;
    int64_t  downloads;
} ct_proj;
typedef struct {
    wchar_t* id;
    wchar_t* name;
    wchar_t* url;
    wchar_t* filename;
    uint64_t size;
} ct_ver;
int  ct_search(const wchar_t* query, ct_type type, const wchar_t* mcver,
               const wchar_t* loader, ct_proj** out, int* n, wchar_t* err, size_t errlen);
int  ct_versions(const wchar_t* project_id, const wchar_t* mcver, const wchar_t* loader,
                 ct_ver** out, int* n, wchar_t* err, size_t errlen);
int  ct_download(const ct_ver* v, const wchar_t* dest, dl_progress* prog,
                 wchar_t* err, size_t errlen);
void ct_proj_free(ct_proj* p, int n);
void ct_ver_free(ct_ver* v, int n);
const wchar_t* ct_type_name(ct_type t);
const wchar_t* ct_type_project(ct_type t);   /* "mod"/"shader"/"resourcepack"/"modpack" */

/* ---------------- java auto download (Adoptium Temurin) ---------------- */
int javadl_install(int major, int is64, dl_progress* prog, wchar_t* err, size_t errlen);
wchar_t* javadl_find(int major, int is64);    /* 在 runtime 目录找已安装 java.exe */

/* ---------------- modpack (.mrpack) ---------------- */
int modpack_install(const wchar_t* gamedir, const wchar_t* mrpack_path, int isolate,
                    dl_progress* prog, wchar_t* err, size_t errlen);

/* ---------------- server list (servers.dat) ---------------- */
typedef struct {
    wchar_t* name;
    wchar_t* ip;
    int hidden;
    wchar_t* icon;  /* base64 png，可选 */
} srv_entry;

void server_list_free(srv_entry* e, int n);
int servers_load(const wchar_t* gamedir, srv_entry** out, int* n);
int servers_save(const wchar_t* gamedir, const srv_entry* e, int n);
int servers_load_path(const wchar_t* path, srv_entry** out, int* n);
int servers_save_path(const wchar_t* path, const srv_entry* e, int n);
static int srv_add_arg(const srv_entry* e, wsa* args);  /* 内部用：把 --server ip 加入启动参数 */

/* ---------------- mod loader (Fabric/Quilt/Forge/NeoForge) ---------------- */
typedef enum { LD_FABRIC = 0, LD_QUILT = 1, LD_FORGE = 2, LD_NEOFORGE = 3 } ld_kind;
typedef struct {
    wchar_t* version;   /* 加载器版本，如 0.16.10 / 47.4.28 */
    int      stable;    /* 1 = 稳定版 */
} ld_entry;
int  loader_list(ld_kind kind, const wchar_t* mcver, ld_entry** out, int* n,
                 wchar_t* err, size_t errlen);
/* mcver=用于查询的原版版本号；dir_id=实际游戏目录名（自定义版本与 mcver 不同，
 * 加载器 profile 的 inheritsFrom 需指向 dir_id）。 */
int  loader_install(ld_kind kind, const wchar_t* gamedir, const wchar_t* mcver,
                    const wchar_t* dir_id, const wchar_t* loader,
                    dl_progress* prog, wchar_t* err, size_t errlen);
void loader_list_free(ld_entry* e, int n);
const wchar_t* loader_kind_name(ld_kind k);     /* "Fabric"/"Quilt"/"Forge"/"NeoForge" */

/* ---------------- auth (微软正版登录) ---------------- */
int msa_devicecode(const wchar_t* client_id, wchar_t* user_code, size_t ucsz,
                   wchar_t* device_code, size_t dsz, wchar_t* verify_url, size_t vsz,
                   wchar_t* err, size_t errsz,
                   int* interval_out, int* expires_out);
int msa_poll_token(const wchar_t* client_id, const wchar_t* device_code,
                   int interval_sec,
                   wchar_t* access, size_t asz, wchar_t* refresh, size_t rsz,
                   wchar_t* err, size_t errsz);
int msa_refresh(const wchar_t* client_id, const wchar_t* refresh,
                wchar_t* access, size_t asz, wchar_t* refresh_new, size_t rsz,
                wchar_t* err, size_t errsz);
int msa_token_login(const wchar_t* token, wchar_t* uuid, size_t usz, wchar_t* name, size_t nsz,
                    wchar_t* xuid, size_t xsz, wchar_t* err, size_t errsz);
int msa_restore(const wchar_t* client_id, const wchar_t* refresh,
                wchar_t* uuid, size_t usz, wchar_t* name, size_t nsz,
                wchar_t* xuid, size_t xsz, wchar_t* access_out, size_t asz,
                wchar_t* refresh_new, size_t rsz, wchar_t* err, size_t errsz);

/* ---------------- disk cleanup ---------------- */
typedef struct {
    wchar_t* label;
    wchar_t* root;         /* absolute root to scan/delete */
    int      pattern;      /* 0: whole dir, 1: matching files in root */
    uint64_t bytes;
    int      files;
    int      dirs;
} ccat;
typedef int (*clean_progress)(void* user, int files, uint64_t bytes);
int disk_scan(const wchar_t* gamedir, ccat** out, int* n, clean_progress cb, void* user);
uint64_t disk_clean_one(ccat* c, clean_progress cb, void* user);
void ccat_free(ccat* c, int n);
wchar_t* fmt_bytes(uint64_t b);            /* "1.2 MB" (malloc) */

/* ---------------- uuid ---------------- */
void md5_of(const unsigned char* data, size_t len, unsigned char digest[16]);
void uuid_offline(const wchar_t* player, wchar_t* out, size_t outcap);

/* ---------------- run child (capture output) ---------------- */
int run_capture(const wchar_t* exe, const wchar_t* argline, char* out, size_t cap, int timeout_ms);

/* ---------------- 远程联机（房间 / 中转 / 直连） ----------------
 * 设计目标：不安装虚拟网卡，也不要求双方装驱动。主机把 Minecraft 的
 * “对局域网开放”端口接入房间；玩家加入后在本机得到一个 TCP 代理，
 * 并由启动器模拟 Minecraft 的 LAN 组播公告，游戏多人列表里会直接出现房间，
 * 体验接近 Radmin LAN 的“加入后就能在局域网列表看到”。 */
#define NET_MODE_DIRECT 0
#define NET_MODE_RELAY  1

typedef struct {
    int      active;        /* 1 = 房间正在运行/启动中 */
    int      is_host;       /* 1 = 我是主机，0 = 我是加入方 */
    int      mode;          /* NET_MODE_DIRECT / NET_MODE_RELAY */
    int      peers;         /* 已连接玩家数（主机视角） */
    int      mc_port;       /* 主机：Minecraft LAN 端口；加入方：本机代理端口 */
    wchar_t  room[32];      /* 房间码 */
    wchar_t  invite[1024];  /* 邀请码（主机） */
    wchar_t  address[256];  /* 加入方：127.0.0.1:port；主机：端口说明 */
    wchar_t  relay[256];    /* 中转地址 */
    wchar_t  status[512];   /* 当前状态 / 错误 */
} net_status;

#define NET_EV_STATUS   1
#define NET_EV_LOG      2
#define NET_EV_STOPPED  3
typedef void (*net_event_cb)(int ev, const wchar_t* msg, void* user);

void net_set_event_cb(net_event_cb cb, void* user);
int  net_create_host(int mode, const wchar_t* relay_addr, int mc_port, int listen_port,
                     const wchar_t* logs_dir, wchar_t* err, size_t errlen);
int  net_join(const wchar_t* invite, wchar_t* err, size_t errlen);
void net_stop(void);
int  net_active(void);
void net_get_status(net_status* st);
int  net_copy_invite(wchar_t* out, size_t cap);
void net_clipboard_copy(HWND owner, const wchar_t* text);
int  net_relay_run(int port);        /* 阻塞运行中转服务器（CLI --relay） */
int  net_selftest(void);             /* 回环自检：中转 + 主机 + 加入 + 数据转发 */
int  net_cli_host(int mode, const wchar_t* relay, int mc_port, int seconds);
int  net_cli_join(const wchar_t* invite, int seconds);

/* ---------------- UPnP（直连房间自动开端口） ---------------- */
int  upnp_map_tcp(int port, wchar_t* ext_ip, size_t cap);  /* 1 = 映射成功 */
void upnp_unmap_tcp(int port);

/* ---------------- ui / cli ---------------- */
extern HWND g_main, g_log, g_status, g_list, g_btn_launch;
void ui_init(HINSTANCE h);
int  ui_run(void);
void ui_log(const wchar_t* fmt, ...);
void ui_status(const wchar_t* fmt, ...);
void cli_print(const wchar_t* s);

#endif /* LLCL_H */
