
#include <stdio.h>
#include "memory.h"

// 从 /proc/meminfo 获取内存信息
int get_memory_info(MemoryInfo *mem)
{
    if (mem == NULL)
    {
        fprintf(stderr, "MemoryInfo pointer is NULL\n");
        return -1;
    }

    FILE *fp = fopen("/proc/meminfo", "r");

    if (fp == NULL)
    {
        perror("fopen /proc/meminfo");
        return -1;
    }

    char line[256];
    unsigned long total = 0;
    unsigned long available = 0;

    // 逐行读取内存信息
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        if (sscanf(line, "MemTotal: %lu kB",
                   &total) == 1)
        {
            continue;
        }

        if (sscanf(line, "MemAvailable: %lu kB",
                   &available) == 1)
        {
            continue;
        }
    }

    fclose(fp);

    // 检查数据有效性
    if (total == 0 || available > total)
    {
        fprintf(stderr, "Invalid memory information\n");
        return -1;
    }

    // 保存内存数据
    mem->total = total;
    mem->available = available;
    mem->used = total - available;

    // 计算内存使用率
    mem->usage = (double)mem->used / total * 100.0;

    return 0;
}
