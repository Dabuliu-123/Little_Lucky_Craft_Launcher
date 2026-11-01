LLcl 1.1.0 - 极简 Minecraft Java 启动器（发行版）

系统要求
- Windows 7 / 8 / 10 / 11（64 位）
- 无需 .NET、无需预装 Java；启动器会在需要时自动下载对应 Java（需要联网）
- 单个 LLcl.exe 约 333KB，空闲内存约 2.3MB

首次使用
1. 双击 LLcl.exe。
2. “设置”里确认游戏目录（默认是 exe 同级的 .minecraft，首次启动会自动创建）。
3. 列表里没有游戏版本时，点“下载与安装”，选择版本安装（自带资源、库和对应 Java）。
4. 选中版本，点“启动游戏”。

常用功能
- 下载与安装：游戏本体 + 资源 + 自动 Java，也能安装 Fabric / Quilt / Forge / NeoForge。
- 内容中心：Mod / 光影 / 资源包 / 整合包；支持安装本地 .mrpack。
- 账号与登录：离线玩家名在“设置”修改；支持微软正版设备码登录，启动时自动续期。
- 磁盘清理：清理 logs、crash-reports、下载缓存、JVM 崩溃残留。
- 版本隔离：开启后每个版本的存档 / 模组 / 配置独立在 versions/<版本>/instance。

注意事项
- 首次下载 26.x 需要较多资源文件（约 400MB+），请保持网络畅通。
- 如果杀毒软件拦截，请把 LLcl.exe 加入信任；启动器只访问 Mojang / BMCLAPI / Modrinth / 微软登录接口。
- 配置保存在 exe 同级的 LLcl.ini，删除它即可恢复默认设置。
- 游戏日志在 <游戏目录>\logs\llcl-launch.log。

本目录只包含发行版文件；源码、构建脚本和测试环境在上一级 dev 目录。