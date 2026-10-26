
#ifndef MEMORY_H
#define MEMORY_H

// 内存信息结构体
typedef struct
{
    unsigned long total;
    unsigned long available;
    unsigned long used;
    double usage;
} MemoryInfo;

// 获取 Linux 内存信息
int get_memory_info(MemoryInfo *mem);

#endif
