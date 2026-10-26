
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/utsname.h>

#include "cpu.h"
#include "memory.h"
#include "disk.h"
#include "process.h"

// 运行状态：1 表示继续运行，0 表示退出
static volatile sig_atomic_t running = 1;

// 接收到 Ctrl+C 后修改运行状态
static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

// 注册信号处理函数
static int setup_signals(void)
{
    struct sigaction sa = {0};

    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1)
    {
        perror("sigaction");
        return -1;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1)
    {
        perror("sigaction");
        return -1;
    }

    return 0;
}

// 显示系统监控信息
static int display_monitor(void)
{
    struct utsname system_info;
    MemoryInfo mem;
    DiskInfo disk;

    if (uname(&system_info) == -1)
    {
        perror("uname");
        return -1;
    }

    // CPU 采样需要约 1 秒
    double cpu_usage = get_cpu_usage();

    // 收到退出信号后不再进行下一轮采集
    if (!running)
        return 0;

    if (cpu_usage < 0)
    {
        fprintf(stderr, "CPU read failed\n");
        return -1;
    }

    if (get_memory_info(&mem) != 0)
    {
        fprintf(stderr, "Memory read failed\n");
        return -1;
    }

    if (get_disk_info(&disk) != 0)
    {
        fprintf(stderr, "Disk read failed\n");
        return -1;
    }

    int process_count = get_process_count();

    if (process_count < 0)
    {
        fprintf(stderr, "Process count failed\n");
        return -1;
    }

    // 清屏并移动光标到左上角
    printf("\033[2J\033[H");

    time_t now = time(NULL);
    struct tm local_time;

    printf("====================================\n");
    printf("      Linux System Monitor v1.5\n");
    printf("====================================\n");

    if (localtime_r(&now, &local_time) != NULL)
    {
        char time_buffer[64];

        if (strftime(time_buffer, sizeof(time_buffer),
                     "%Y-%m-%d %H:%M:%S",
                     &local_time) > 0)
        {
            printf("Time: %s\n", time_buffer);
        }
    }

    printf("System: %s\n", system_info.sysname);
    printf("Host: %s\n", system_info.nodename);
    printf("Kernel: %s\n", system_info.release);
    printf("Process ID: %ld\n", (long)getpid());

    printf("------------------------------------\n");

    printf("CPU Usage: %.2f%%\n", cpu_usage);

    printf("------------------------------------\n");

    printf("Total Memory: %.2f MB\n",
           mem.total / 1024.0);
    printf("Used Memory: %.2f MB\n",
           mem.used / 1024.0);
    printf("Available Memory: %.2f MB\n",
           mem.available / 1024.0);
    printf("Memory Usage: %.2f%%\n",
           mem.usage);

    printf("------------------------------------\n");

    printf("Disk Total: %.2f GB\n", disk.total_gb);
    printf("Disk Used: %.2f GB\n", disk.used_gb);
    printf("Disk Available: %.2f GB\n",
           disk.available_gb);
    printf("Disk Usage: %.2f%%\n", disk.usage);

    printf("------------------------------------\n");

    printf("Process Count: %d\n", process_count);

    printf("====================================\n");
    printf("Press Ctrl+C to exit\n");

    fflush(stdout);

    return 0;
}

int main(void)
{
    if (setup_signals() != 0)
        return 1;

    printf("Starting Linux System Monitor...\n");

    while (running)
    {
        if (display_monitor() != 0)
        {
            fprintf(stderr, "Monitoring stopped due to error\n");
            return 1;
        }
    }

    printf("\nLinux System Monitor stopped safely.\n");

    return 0;
}
