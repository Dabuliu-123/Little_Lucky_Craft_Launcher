LLcl 1.2.0 — 开发版说明

目录
- src\            源码（.cpp/.h，统一 UTF-8）
- build.ps1       一键构建：读取 LLCL_VERSION，输出 ..\release\<版本>\ 并打包 zip
- release-files\  发行版附带模板（LLcl.ini / README.txt）
- build\          编译中间产物
- runtime\        开发用 Java 8/17/21 缓存（LLCL_RUNTIME_DIR 可复用）
- testdata\       测试游戏目录（含 26.3、1.20.1、Forge/Fabric/Quilt 等）
- e2e\            端到端测试
- logs-root\      历史日志
- installer-root.log 历史安装器日志

构建
  powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1

输出
  ..\release\1.2.0\LLcl.exe
  ..\release\1.2.0\LLcl.ini
  ..\release\1.2.0\README.txt
  ..\release\1.2.0\LLcl-1.2.0-win64.zip

自检
  ..\release\1.2.0\LLcl.exe --selftest .\testdata\mc
  ..\release\1.2.0\LLcl.exe --scan .\testdata\mc
  ..\release\1.2.0\LLcl.exe --launch .\testdata\mc 26.3 "C:\Program Files\Java\jdk-25.0.4.1\bin\java.exe" 2048 Steve

调试环境变量
  LLCL_RUNTIME_DIR=<dir>   复用 Java
  LLCL_DL_THREADS=<n>      下载并发
  LLCL_DEBUG_CMD=<path>    导出最终启动命令行

发布
  每个版本一个文件夹：release\<版本>\ 内放单文件 exe、LLcl.ini、README.txt 和该版本 zip。
