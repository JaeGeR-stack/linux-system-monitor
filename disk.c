
#include <stdio.h>
#include <sys/statvfs.h>
#include "disk.h"

// 获取 Linux 根文件系统磁盘信息
int get_disk_info(DiskInfo *disk)
{
    if (disk == NULL)
    {
        fprintf(stderr, "DiskInfo pointer is NULL\n");
        return -1;
    }

    struct statvfs fs;

    if (statvfs("/", &fs) != 0)
    {
        perror("statvfs");
        return -1;
    }

    double block_size = (double)fs.f_frsize;

    double total = (double)fs.f_blocks * block_size;
    double free_space = (double)fs.f_bfree * block_size;
    double available = (double)fs.f_bavail * block_size;

    double used = total - free_space;
    double usable_total = used + available;

    if (total <= 0 || usable_total <= 0)
    {
        fprintf(stderr, "Invalid disk information\n");
        return -1;
    }

    disk->total_gb =
        total / (1024.0 * 1024.0 * 1024.0);

    disk->used_gb =
        used / (1024.0 * 1024.0 * 1024.0);

    disk->available_gb =
        available / (1024.0 * 1024.0 * 1024.0);

    // 与 df 的磁盘使用率计算口径保持接近
    disk->usage = used / usable_total * 100.0;

    return 0;
}
