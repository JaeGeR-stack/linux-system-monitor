
#include <stdio.h>
#include <dirent.h>
#include <ctype.h>
#include "process.h"

// 判断字符串是否全部由数字组成
static int is_numeric(const char *str)
{
    if (str == NULL || *str == '\0')
        return 0;

    while (*str != '\0')
    {
        if (!isdigit((unsigned char)*str))
            return 0;

        str++;
    }

    return 1;
}

// 统计 Linux 当前可见的进程数量
int get_process_count(void)
{
    DIR *dir = opendir("/proc");

    if (dir == NULL)
    {
        perror("opendir /proc");
        return -1;
    }

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL)
    {
        // /proc 下的数字目录通常对应进程 PID
        if (is_numeric(entry->d_name))
        {
            count++;
        }
    }

    if (closedir(dir) != 0)
    {
        perror("closedir /proc");
        return -1;
    }

    return count;
}
