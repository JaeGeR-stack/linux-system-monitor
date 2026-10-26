
#include <stdio.h>
#include <unistd.h>
#include "cpu.h"

// 读取 Linux CPU 累计时间
int get_cpu_info(CpuInfo *cpu)
{
    if (cpu == NULL)
        return -1;

    FILE *fp = fopen("/proc/stat", "r");

    if (fp == NULL)
    {
        perror("fopen /proc/stat");
        return -1;
    }

    char line[256];

    if (fgets(line, sizeof(line), fp) == NULL)
    {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;

    int count = sscanf(
        line,
        "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
        &user,
        &nice,
        &system,
        &idle,
        &iowait,
        &irq,
        &softirq,
        &steal
    );

    if (count != 8)
    {
        fprintf(stderr, "Invalid CPU information\n");
        return -1;
    }

    cpu->total = user + nice + system + idle
               + iowait + irq + softirq + steal;

    cpu->idle = idle + iowait;

    return 0;
}

// 通过两次采样计算 CPU 使用率
double get_cpu_usage(void)
{
    CpuInfo first;
    CpuInfo second;

    if (get_cpu_info(&first) != 0)
        return -1.0;

    sleep(1);

    if (get_cpu_info(&second) != 0)
        return -1.0;

    if (second.total <= first.total ||
        second.idle < first.idle)
        return -1.0;

    unsigned long long total_diff =
        second.total - first.total;

    unsigned long long idle_diff =
        second.idle - first.idle;

    if (idle_diff > total_diff)
        return -1.0;

    return (double)(total_diff - idle_diff)
           / total_diff * 100.0;
}
