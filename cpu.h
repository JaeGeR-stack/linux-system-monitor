
#ifndef CPU_H
#define CPU_H

typedef struct
{
    unsigned long long total;
    unsigned long long idle;
} CpuInfo;

// 读取 CPU 累计时间
int get_cpu_info(CpuInfo *cpu);

// 计算 CPU 使用率
double get_cpu_usage(void);

#endif
