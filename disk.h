
#ifndef DISK_H
#define DISK_H

typedef struct
{
    double total_gb;
    double used_gb;
    double available_gb;
    double usage;
} DiskInfo;

// 获取 Linux 根文件系统磁盘信息
int get_disk_info(DiskInfo *disk);

#endif
