/* LLcl - mem.c : physical memory probing + automatic JVM heap tiers */
#include "llcl.h"

uint64_t total_phys_bytes(void) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) return ms.ullTotalPhys;
    return 0;
}

int auto_heap_mb(void) {
    uint64_t bytes = total_phys_bytes();
    uint64_t gb = bytes / (1024ULL * 1024ULL * 1024ULL);
    uint64_t mb = bytes / (1024ULL * 1024ULL);
    if (mb == 0) return 512;
    if (mb < 1536) return 384;    /* < 1.5 GB 老机器: 384MB */
    if (mb < 2048) return 512;    /* 1.5-2GB */
    if (mb < 4096) return 1024;   /* 2-4GB */
    if (mb < 8192) return 2048;   /* 4-8GB */
    if (mb < 16384) return 3072;  /* 8-16GB */
    if (gb >= 32) return 6144;
    return 4096;
}

int auto_xms_mb(int heap) {
    int xms = heap / 4;
    if (xms < 256) xms = 256;
    if (xms > 2048) xms = 2048;
    return xms;
}

/* 压缩所有堆（含 wininet/CRT 等组件创建的独立堆）并把不常用页换出，
 * 降低任务管理器显示的内存（操作结束后调用）。 */
void mem_trim(void) {
    HANDLE heaps[128];
    DWORD n = GetProcessHeaps(128, heaps);
    for (DWORD i = 0; i < n; i++)
        HeapCompact(heaps[i], 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}
