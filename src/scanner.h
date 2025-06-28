// src/scanner.h - 扫描器头文件
#ifndef SCANNER_H
#define SCANNER_H

#include "scan_options.h"
#include "time_categories.h"
#include <sys/time.h>

// 设备几何信息结构
typedef struct {
    unsigned int sector_size;      // 逻辑扇区大小
    unsigned long total_sectors;   // 总扇区数
} DeviceGeometry;

// 扫描进度结构
typedef struct {
    unsigned long current_sector;
    unsigned long sectors_scanned;
    unsigned long total_sectors;
    double progress_percent;
    double sectors_per_second;
    int estimated_remaining_sec;
    int last_read_time;
    TimeCategoryType last_category;
    struct timeval start_time;
} ScanProgress;

// 主扫描函数
int scan_device(const ScanOptions* opts);

// 位置解析函数（从 scan_options.c 中声明）
int parse_positions(const ScanOptions *opts, unsigned long total_sectors,
                   unsigned long *start_sector, unsigned long *end_sector);

#endif // SCANNER_H
