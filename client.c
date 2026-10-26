
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include "cpu.h"
#include "memory.h"
#include "disk.h"
#include "process.h"

#define SERVER_IP "127.0.0.1"
#define PORT 8888
#define BUFFER_SIZE 1024

static volatile sig_atomic_t running = 1;

// 处理退出信号
static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

static int setup_signals(void)
{
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1)
    {
        perror("sigaction");
        return -1;
    }

    return 0;
}

// 完整发送一条消息
static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t n = send(fd, data + sent,
                         len - sent, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
        {
            if (!running)
                return -1;
            continue;
        }

        if (n <= 0)
        {
            if (n < 0)
                perror("send");
            return -1;
        }

        sent += (size_t)n;
    }

    return 0;
}

// 建立 TCP 连接
static int connect_server(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, SERVER_IP,
                  &addr.sin_addr) != 1)
    {
        fprintf(stderr, "Invalid server IP\n");
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr,
                sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

int main(void)
{
    if (setup_signals() != 0)
        return 1;

    int fd = connect_server();

    if (fd < 0)
        return 1;

    printf("Connected to Linux Monitor Server!\n");
    printf("Sending monitoring data continuously...\n");
    printf("Press Ctrl+C to exit.\n");

    int result = 0;
    unsigned long sequence = 0;

    while (running)
    {
        MemoryInfo mem;
        DiskInfo disk;

        // CPU 模块内部包含约 1 秒采样
        double cpu_usage = get_cpu_usage();

        if (!running)
            break;

        if (cpu_usage < 0 ||
            get_memory_info(&mem) != 0 ||
            get_disk_info(&disk) != 0)
        {
            fprintf(stderr, "Collection failed\n");
            result = 1;
            break;
        }

        int count = get_process_count();

        if (count < 0)
        {
            fprintf(stderr, "Process collection failed\n");
            result = 1;
            break;
        }

        char buffer[BUFFER_SIZE];

        int len = snprintf(
            buffer, sizeof(buffer),
            "{\"sequence\":%lu,"
            "\"cpu_usage\":%.2f,"
            "\"memory_usage\":%.2f,"
            "\"disk_usage\":%.2f,"
            "\"process_count\":%d}\n",
            ++sequence,
            cpu_usage,
            mem.usage,
            disk.usage,
            count
        );

        if (len < 0 || (size_t)len >= sizeof(buffer))
        {
            fprintf(stderr, "Buffer too small\n");
            result = 1;
            break;
        }

        if (send_all(fd, buffer, (size_t)len) != 0)
        {
            if (running)
            {
                fprintf(stderr, "Connection lost\n");
                result = 1;
            }
            break;
        }

        printf("Sent #%lu: %s", sequence, buffer);
        fflush(stdout);
    }

    close(fd);
    printf("\nClient stopped safely.\n");

    return result;
}
