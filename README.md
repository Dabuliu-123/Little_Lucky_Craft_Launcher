# LLcl 1.4.0 — 极简 Minecraft Java 启动器

> 纯 C++ / Win32 编写、`/MT` 静态链接、零 .NET / 零第三方运行时的单文件启动器。
> 面向装不了 .NET、内存小、硬盘紧张的老机器；支持 Windows 7 及以上 64 位系统。

## 目录结构（发行版 / 开发版）

每个版本的发行版独立放在 `release\<版本号>\`，目录内同时保留**单文件 exe** 和**压缩包**。

```
F:\llcl\
├── release\
│   ├── 1.1.0\                         # 历史发行版
│   │   ├── LLcl.exe
│   │   ├── LLcl.ini
│   │   ├── README.txt
│   │   └── LLcl-1.1.0-win64.zip
│   ├── 1.2.0\                         # 历史发行版
│   │   ├── LLcl.exe
│   │   ├── LLcl.ini
│   │   ├── README.txt
│   │   └── LLcl-1.2.0-win64.zip
│   ├── 1.3.0\                         # 历史发行版
│   │   ├── LLcl.exe
│   │   ├── LLcl.ini
│   │   ├── README.txt
│   │   └── LLcl-1.3.0-win64.zip
│   └── 1.4.0\                         # 当前发行版
│       ├── LLcl.exe                   # 单文件启动器（约 433KB）
│       ├── LLcl.ini                   # 默认配置模板
│       ├── README.txt                 # 用户使用说明
│       └── LLcl-1.4.0-win64.zip       # 发行压缩包
├── dev\                               # 开发版
│   ├── src\                           # 21 个 .cpp + llcl.h + jsmn.h
│   ├── build.ps1                      # 一键构建（自动版本号 + 打包）
│   ├── release-files\                 # 发行版附带的 ini/README 模板
│   ├── build\                         # 中间产物（.obj / run.bat）
│   ├── runtime\                       # 开发用 Java 缓存（Java 8/17/21/25）
│   ├── testdata\                      # 测试游戏目录（含 26.3 等版本）
│   ├── e2e\                           # 端到端测试
│   ├── logs-root\                     # 历史日志
│   └── installer-root.log             # 历史 installer 日志
└── README.md
```

`dev\build.ps1` 会从 `src\llcl.h` 读取 `LLCL_VERSION`，自动输出到 `release\<版本号>\`，
复制 `dev\release-files\LLcl.ini` 与 `README.txt`，并生成同目录的 zip。

## 实测指标（1.4.0）

| 项目 | 数值 |
|---|---|
| LLcl.exe 体积 | **433,152 字节（约 433KB）**，`/MT` 静态链接，低于 500KB 目标 |
| 空闲常驻内存 | 私有提交 **约 2.3MB**，工作集 **约 1.1MB** |
| 启动阶段峰值工作集 | **约 15.4MB**（实测，低于 19MB） |
| 联机房间运行峰值 | **约 15.4MB**（实测，含对话框/中转/代理线程，低于 19MB） |
| 冷启动到界面 | < 200ms（本机） |
| 系统兼容 | 目标 Win7 – Win11（仅用 Win7 已有 API，`SetProcessDPIAware`） |
| 联机自检 | `LLcl.exe --nettest`：中转协议、直连、加入方代理、双向转发全部通过 |
| 26.3 启动实测 | 原生库 4ms、命令行 8ms、进程创建 24ms，Java 25 进入标题界面 |

> 下载线程数默认 8（省内存），可在设置中改 16（更快、占更多 TLS 缓冲）。

## 1.4.0：启动兼容修复 + 下载界面简化

- **启动修复（Java 向后兼容匹配）**：1.20.1 需 Java 17、1.16 需 Java 8 等版本，
  之前要求“精确匹配”Java 主版本号，导致装了更高版本 Java 也误报“找不到 Java”打不开。
  现改为：优先精确匹配，否则选 **≥ 所需中最低版本的 64 位 Java**（Java 向后兼容）；
  运行时与本机检测、自动下载逻辑同步修正。
  - 1.20.1（需 17）：装 21 可直接跑。
  - 1.16（需 8）：正确匹配 / 自动下载 Java 8，避免用过高 Java 导致**黑屏、界面文字/
    语言异常**（花屏马赛克）。
  - 26.3 快照（需 25）：自动下载 Java 25，且下载回退从“只下 JRE”改为 **JRE→JDK 两级
    回退**（新版 Java 25 不再单独发布 JRE），修复 Mojang 图标后闪退。
- **下载界面 PCL 化（去冗余）**：一屏直接选 **原版 / 模组加载器**，省去“先下原版→再
  另开窗口装加载器”的多步操作。选模组加载器（Fabric/Forge/NeoForge/Quilt）后点“下载”
  即自动补原版 + 取最新稳定加载器 + 安装，一步完成。

## 1.3.0 新增：远程联机（像 Radmin LAN 一样进 LAN 列表）

启动器不安装虚拟网卡/驱动（那需要管理员权限和签名驱动，和单文件 <500KB 冲突），
而是用应用层方案达到接近的使用体验：

- 主机在游戏里“对局域网开放”；
- 启动器把该 TCP 端口接入房间；
- 加入方在本机得到一个 `127.0.0.1:25565` 代理；
- 启动器同时模拟 Minecraft 的 LAN 组播公告（`224.0.2.60:4445`），
  所以加入方打开“多人游戏”就能在**局域网列表**里看到房间，也可以直接连接本机代理地址。

### 两种模式

| 模式 | 适用场景 | 说明 |
|---|---|---|
| 直连 | 有公网 IP、路由器支持 UPnP，或同一局域网 | 主机监听一个 TCP 端口，尝试 UPnP 自动映射；不行就手动映射。邀请码带公网/局域网地址。 |
| 中转 | 双方都在 NAT 后面，没有公网 IP | 需要一台双方都能访问的机器运行 `LLcl.exe --relay 25566`；主机和玩家都主动连中转，免公网 IP。 |

### 使用步骤

主机：
1. 进入世界 → Esc → “对局域网开放”。
2. 启动器 → “远程联机（房间 / 服务器）”。
3. 选“直连”或“中转”；直连时 MC 端口**留空会自动从 `latest.log` 读取**。
   中转时填写中转服务器地址（如 `1.2.3.4:25566`）。
4. 点“创建房间”，点“复制邀请码”发给朋友。

玩家：
1. 打开“远程联机”，粘贴邀请码，点“加入房间”。
2. 打开 Minecraft → 多人游戏 → 局域网列表会出现房间；也可直接连接界面上的
   `127.0.0.1:端口`。
3. 联机期间保持启动器运行。

### 实现要点（`src/netroom.cpp`、`src/upnp.cpp`）

- 直连协议：`JOIN <token>` 鉴权后双向 TCP 转发；端口 25566 起自动找空闲。
- 中转协议：主机 `HOST`、玩家 `JOIN`、主机按 `PEER` 再 `BIND` 建立数据通道；
  房间码 6 位 + 10 位令牌，防止只靠房间码误入。
- 中转服务器自带房间表（16 房间 × 8 人）、心跳、断线清理；
  单连接一个线程（64–128KB 栈）+ `select` 双向转发。
- UPnP：SSDP 发现 IGD → 读取描述 → `AddPortMapping` / `GetExternalIPAddress` /
  `DeletePortMapping`，失败自动回退到“手动映射/中转”。
- LAN 公告：每 1.5 秒发送 `[MOTD]…[/MOTD][AD]<端口>[/AD]` 到组播和本机回环，
  不修改游戏、不需要 mod。
- 不依赖 zlib1.dll：`servers.dat` 的 gzip 使用内置 raw DEFLATE 解压 +
  gzip stored 块压缩；顺带修复了原服务器列表读写失效的问题。

### 安全与限制

- 中转是**明文 TCP**，只应提供给朋友；房间码/令牌用于防误入，不提供加密。
- 直连模式依赖主机可被访问：公网 IP / UPnP / 手动端口映射，三者至少一个；
  Windows 防火墙需要允许 `LLcl.exe` 入站。
- 没有虚拟网卡，所以不会出现在“网络适配器”里，也不能用于非 Minecraft 程序。
- 中转服务器断线后房间关闭，需要重建（当前不做自动重连）。

## 1.2.0：版本管理

主窗口选中版本后点 **“版本管理（模组 / 光影 / 数据包）”**：

- **查看当前版本内容**：模组 `mods\`、光影 `shaderpacks\`、数据包 `datapacks\`；
  开启版本隔离时读取 `versions\<版本>\instance\...`。
- **添加本地模组**：文件选择器支持多选，复制到当前版本的 `mods\`；光影/数据包也可添加。
- **启用 / 禁用**：在 `xxx.jar` 与 `xxx.jar.disabled` 之间切换（光影/数据包同理）。
- **删除 / 刷新 / 打开文件夹**：删除前确认；支持一次全部启用或全部禁用。
- 数据包提示：世界专用数据包位于 `saves\<世界>\datapacks`。

## 1.1.0 主要修复 / 完成项（历史）

### 26.3 无法启动（核心）
- **下载入口**：BMCLAPI 的 `/version/<id>/json` 对新版本可能 404；现在版本清单同时拉取
  **BMCLAPI + Mojang 官方**并按 id 合并；版本 JSON 先试 BMCL，失败用官方 URL；
  校验 JSON 内容、client.jar 大小、library 大小，拒绝镜像返回的 200 错误页。
- **资源索引**：26.x 缺资源对象会崩溃；索引下载/校验失败明确报错，索引有
  `assetIndex.sha1` 时构造官方包地址重试。
- **原生库布局**：26.x 的 `-Djava.library.path` 指向 `natives/java`；按 JVM 模板
  识别子目录，用 native jar 指纹 + dll 数量判断是否完整重解压。
- **Java 25**：按 `javaVersion.majorVersion` 精确选择 Java 25；没有时自动下载
  Adoptium Temurin 25（清华镜像优先，API 回退）。

### 正版登录
- 保存并使用真正的 Minecraft access token；从 XSTS 的 `xid` 解析 **XUID**；
  有 `refresh_token` 时在启动线程里静默续期，失败沿用旧凭据，完全失败才回退离线。
- 修复设备码响应 `interval`/`expires_in` 是数字却被当字符串读取的问题。

### 模组 / 整合包
- Forge / NeoForge installer 用错 Java：官方字段是 `majorVersion`，已修复。
- 安装器下载改为流式写盘并校验 zip 魔数，避免 10–30MB installer 整个进内存。
- 修复自定义版本名比原版短时 `str_replace_all` 的 `size_t` 下溢。
- 整合包版本隔离：mods/overrides/配置写入新加载器版本的 `versions/<id>/instance`。
- NeoForge 版本目录识别改为读 JSON 的 `inheritsFrom`。
- 整合包内文件路径拒绝绝对路径 / 盘符 / `..` 穿越。
- 内容中心支持 Modrinth 整合包搜索 + 一键下载，并保留“安装本地 .mrpack”。

### 界面整合
- 主窗口：启动游戏、设置、下载与安装、内容中心、版本管理、账号与登录、磁盘清理、
  远程联机。
- 玩家名移到“设置”，登录状态在主窗口一行摘要；微软登录窗口改为“账号与登录”。
- “检测 Java”不再单独占按钮：启动时自动检测，设置里有“重新检测”。

### 其他 bug
- JSON 字符串解码双重编码：中文/日文等非 ASCII 字符乱码；`jstrdup` 直接输出 UTF-8，
  并支持代理对。
- `last_version` 首次运行永远不保存；`run_capture` 先等进程再读管道会死锁；
  断点续传收到 HTTP 416 不再重试 8 次后删完整文件；盘符根目录 `C:\` 尾斜杠；
  高 DPI 换算；磁盘清理递归泄漏/junction 环/JVM 残留误删。

## 功能

- **启动游戏**：版本扫描、`inheritsFrom` 递归合并、规则过滤（含 `os.versionRange`、
  `os.arch`）、natives 解压、modern/legacy 参数、命令行拼接（>30KB 自动 `@argfile`）
- **远程联机**：直连 / 中转、UPnP、邀请码、加入方本机代理、Minecraft LAN 公告
- **版本管理**：查看/添加/启停/删除 模组、光影、数据包
- **离线 / 正版登录**：离线 UUID v3；微软设备码 OAuth + 手填 Token；refresh_token 自动续期
- **下载游戏**：BMCLAPI 镜像优先、官方回退；8/16 线程并发；自定义版本名；自动检测并下载 Java
- **模组加载器**：Fabric / Quilt / Forge / NeoForge；Forge 安装器依赖预下载 + BMCL maven 镜像
- **内容中心**：Mod / 光影 / 资源包 / 整合包（Modrinth），可安装到共享目录或指定版本实例
- **整合包**：`.mrpack` 安装，支持 Fabric/Quilt/Forge/NeoForge，支持版本隔离
- **服务器列表**：servers.dat 读写、局域网扫描、添加/删除、直连启动
- **运行内存优化**：按物理内存自动分档堆大小，按 Java 版本注入 GC 参数，启动前 trim
- **磁盘清理**：logs / crash-reports / downloads / tmp / webcache / JVM 崩溃残留多选清理
- **版本隔离**：每个版本的存档/模组/配置独立于 `versions/<版本>/instance`

## 构建

前置：Visual Studio 2022（MSVC + Windows SDK）。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File F:\llcl\dev\build.ps1
```

脚本会自动查找 `vcvars64.bat`，以 `cl /O1 /MT /std:c++17 /utf-8` 编译，
输出 `release\<LLCL_VERSION>\LLcl.exe`，并生成 `LLcl-<版本>-win64.zip`。
新增的 `netroom.cpp` / `upnp.cpp` 会被 `src\*.cpp` 自动包含；链接 `ws2_32.lib`。

若报 `LNK1104`（exe 被占用）：

```powershell
Get-Process LLcl,java -ErrorAction SilentlyContinue | Stop-Process -Force
```

## 使用

双击 `release\1.4.0\LLcl.exe`。首次启动在 exe 同级创建 `.minecraft` 作为默认游戏目录；
在“设置”里可改游戏目录、内存、玩家名、版本隔离与内存优化。选版本后点“启动游戏”；
需要管理模组/光影/数据包时点“版本管理”；需要联机时点“远程联机”。

### 开发环境变量

- `LLCL_RUNTIME_DIR=<目录>`：复用该目录下已下载的 Java（开发时指向 `dev\runtime`）
- `LLCL_DL_THREADS=<n>`：覆盖下载并发数（默认 8）
- `LLCL_DEBUG_CMD=<path>`：把最终启动命令行写入该文件

### CLI 模式

```
LLcl.exe --help
LLcl.exe --scan <gamedir>                          # 列出版本及库完整性
LLcl.exe --detectjava                              # 探测 Java
LLcl.exe --manifest                                # 拉取并合并版本清单（诊断下载源）
LLcl.exe --download <dir> <ver> [assets 0/1]       # 下载安装（含自动 Java）
LLcl.exe --launch <gamedir> <ver> <java> <memMB> <player> [isolate]
LLcl.exe --uuid <player>                           # 离线 UUID
LLcl.exe --mem                                     # 自动调参结果
LLcl.exe --selftest <gamedir>                      # 内置自检
LLcl.exe --srvtest <gamedir>                       # 读取 servers.dat 并打印
LLcl.exe --relay [port]                            # 运行中转服务器（默认 25566）
LLcl.exe --net-host direct|relay <relay|-> <mcport> <sec>  # 命令行开房
LLcl.exe --net-join <invite> [sec]                 # 命令行加入房间
LLcl.exe --nettest                                 # 联机回环自检
```

中转服务器部署示例（有公网 IP 的 Windows 机器）：

```powershell
LLcl.exe --relay 25566
# 防火墙/安全组放行 TCP 25566；主机和玩家在“中转服务器”里填 公网IP:25566
```

## 已知限制

- **下载源依赖**：版本 JSON 的官方 URL 在 `piston-meta.mojang.com` /
  `launchermeta.mojang.com`；资源索引也来自官方包地址。这些域名不可达时下载会明确失败。
- **资源索引失败会中止安装**：26.x 缺资源会崩溃，所以不再静默跳过；只需要库/客户端时
  可用 CLI `--download <dir> <ver> 1` 跳过 assets。
- **数据包**：版本管理里的“数据包”目录是 `datapacks\`；世界专用数据包仍需放到
  `saves\<世界>\datapacks`。
- **Forge / NeoForge 旧版本**：安装器行为随年代变化大，1.12 及更早需要按实际版本验证。
- **Win7 兼容为代码级保证**，未在实机 Win7 上复测。
- **.mrpack 解压内存**：zip 解析仍会整体读入 `.mrpack`；典型整合包很小，若含超大
  overrides 峰值会随文件增大。
- **16 线程下载**：WinINet/Schannel 每个并发 TLS 连接约 1.5MB 缓冲；小内存机器保持 8。
- **远程联机**：直连需要公网/UPnP/端口映射；中转需要自建可访问的服务器；中转明文；
  没有虚拟网卡，不能给非 Minecraft 程序用；中转断线不自动重连。

## 关键修复记录（历史）

- **XBL 401**：EwA token 需用 `d=` 前缀（不透明 token 用 `t=`）。
- **arm64 natives 覆盖 x64**：收集 natives 时跳过 arm64 jar，只解本机架构。
- **Fabric/Quilt 网络**：Fabric 全链路走 BMCLAPI；Quilt 走 `meta.quiltmc.org`。
- **自动 Java 口径**：精确匹配 major（系统 Java 25 不会被当作 Java 21）。
- **NeoForge 版本前缀 bug**：过滤 1.21.1 时不能用 `strncmp("21.1")`，前缀后必须检查
  下一字符是 `.` 或结尾。
- **1.3.0 servers.dat**：原实现依赖 `zlib1.dll`（Win7 默认没有）且路径多尾斜杠、
  NBT List 解析多读一个 tag；现在内置 gzip/DEFLATE，读写均正常。
