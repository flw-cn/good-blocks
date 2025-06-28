// src/scanner.c - 磁盘扫描核心功能
#include "scanner.h"
#include "device_info/device_info.h"
#include "time_categories.h"
#include "retest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <signal.h>
#include <math.h>

// 全局变量用于信号处理
static volatile int g_scan_interrupted = 0;
static ScanProgress* g_current_progress = NULL;

// 内部函数声明
static void signal_handler(int sig);
static void setup_signal_handlers(void);
static int get_device_geometry(const char *device, DeviceGeometry *geometry);
static int open_device_for_scan(const char* device_path);
static void print_scan_header(const ScanOptions* opts,
                              unsigned long start_sector, unsigned long end_sector);
static void update_progress(ScanProgress* progress, unsigned long current_sector,
                           int read_time_ms, TimeCategoryType category, const TimeCategories* categories, const DeviceGeometry* geometry);
static void update_progress_display(const ScanProgress* progress, const TimeCategories* categories, const DeviceGeometry* geometry);
static void finish_progress_display(void);
static int perform_sector_read(int fd, unsigned long sector, size_t block_size,
                              char* buffer, DeviceGeometry* geometry);
static int handle_suspect_block(unsigned long sector,
                               const ScanOptions* opts, FILE* log_file,
                               const char* device_path);
static void print_final_summary(const ScanProgress* progress, const DeviceGeometry* geometry);
static void log_sector_result(FILE* log_file, unsigned long sector, int read_time_ms,
                             TimeCategoryType category, const char* notes);

/**
 * 信号处理函数
 */
static void signal_handler(int sig) {
    switch (sig) {
        case SIGINT:
        case SIGTERM:
            printf("\n\033[33m【中断】\033[m收到中断信号，正在安全退出...\n");
            g_scan_interrupted = 1;
            break;
        default:
            break;
    }
}

/**
 * 设置信号处理器
 */
static void setup_signal_handlers(void) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/**
 * 获取设备几何信息
 */
static int get_device_geometry(const char *device, DeviceGeometry *geometry) {
    int fd = open(device, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "错误: 无法打开设备 %s: %s\n", device, strerror(errno));
        return -1;
    }

    // 获取逻辑扇区大小（操作系统看到的扇区大小，通常512字节）
    geometry->sector_size = 512;
    if (ioctl(fd, BLKSSZGET, &geometry->sector_size) == -1) {
        fprintf(stderr, "警告: 无法获取逻辑扇区大小，使用默认值 %d 字节\n", geometry->sector_size);
    }

    // 获取物理扇区大小（硬盘真实的最小存储单元，现代SATA硬盘通常4096字节）
    unsigned int physical_sector_size = 512;
    if (ioctl(fd, BLKPBSZGET, &physical_sector_size) == -1) {
        fprintf(stderr, "警告: 无法获取物理扇区大小，假设与逻辑扇区大小相同\n");
        physical_sector_size = geometry->sector_size;
    }

    printf("\033[35m【设备几何】\033[m逻辑扇区大小: %d 字节 (操作系统视角)\n", geometry->sector_size);
    printf("\033[35m【设备几何】\033[m物理扇区大小: %d 字节 (硬盘真实单元)\n", physical_sector_size);

    // 获取总扇区数 (BLKGETSIZE返回的总是以512字节为基准的扇区数)
    geometry->total_sectors = 0;
    if (ioctl(fd, BLKGETSIZE, &geometry->total_sectors) == -1) {
        fprintf(stderr, "错误: 无法获取总扇区数: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    // BLKGETSIZE 返回的扇区数总是以 512 字节为基准，无论实际逻辑扇区大小
    double total_gb = (double)geometry->total_sectors * 512 / (1024*1024*1024);
    printf("\033[35m【设备几何】\033[m设备总扇区数: %lu (以 512 字节计，%.2f GB)\n",
           geometry->total_sectors, total_gb);

    // 如果逻辑扇区大小不是 512，显示换算后的实际逻辑扇区数
    if (geometry->sector_size != 512) {
        unsigned long logical_sectors = geometry->total_sectors * 512 / geometry->sector_size;
        printf("\033[35m【设备几何】\033[m实际逻辑扇区数: %lu (以 %d 字节计)\n",
               logical_sectors, geometry->sector_size);
    }

    close(fd);
    return 0;
}

/**
 * 为扫描打开设备
 */
static int open_device_for_scan(const char* device_path) {
    int fd = open(device_path, O_RDONLY | O_DIRECT | O_SYNC);
    return fd;
}

/**
 * 打印扫描头部信息
 */
static void print_scan_header(const ScanOptions* opts,
                              unsigned long start_sector,
                              unsigned long end_sector) {
    printf("\n\033[1;34m═══════════════════════════════════════════════════════════════\033[0m\n");
    printf("\033[1;34m                    磁盘健康扫描开始                          \033[0m\n");
    printf("\033[1;34m═══════════════════════════════════════════════════════════════\033[0m\n");

    printf("\033[36m扫描设备:\033[0m %s\n", opts->device);
    printf("\033[36m扫描范围:\033[0m 逻辑扇区 %lu - %lu\n", start_sector, end_sector);
    printf("\033[36m扫描扇区数:\033[0m %lu 个逻辑扇区\n", end_sector - start_sector);
    printf("\033[36m块大小:\033[0m %ld 字节 (每次读取)\n", opts->block_size);

    if (opts->sample_ratio < 1.0) {
        unsigned long total_range = end_sector - start_sector;
        unsigned long sample_count = (unsigned long)(total_range * opts->sample_ratio);
        printf("\033[36m采样模式:\033[0m %.2f%% (%lu 个逻辑扇区，%s)\n",
               opts->sample_ratio * 100, sample_count,
               opts->random_sampling ? "随机采样" : "等间距采样");
    } else {
        printf("\033[36m扫描模式:\033[0m 全量扫描\n");
    }

    if (opts->suspect_threshold > 0) {
        printf("\033[36m可疑块阈值:\033[0m %d 毫秒\n", opts->suspect_threshold);
        if (opts->suspect_retries > 0) {
            printf("\033[36m可疑块重测:\033[0m %d 次，间隔 %d 毫秒\n",
                   opts->suspect_retries, opts->suspect_interval);
        }
    }

    if (opts->wait_factor > 0) {
        printf("\033[36m等待因子:\033[0m %d (降低磁盘负载)\n", opts->wait_factor);
    }

    if (opts->log_filename) {
        printf("\033[36m日志文件:\033[0m %s\n", opts->log_filename);
    }

    printf("\033[1;34m═══════════════════════════════════════════════════════════════\033[0m\n");
}

/**
 * 更新扫描进度
 */
static void update_progress(ScanProgress* progress, unsigned long current_sector,
                           int read_time_ms, TimeCategoryType category, const TimeCategories* categories, const DeviceGeometry* geometry) {
    if (!progress) return;

    progress->current_sector = current_sector;
    progress->sectors_scanned++;
    progress->last_read_time = read_time_ms;
    progress->last_category = category;

    // 更新速度统计 (基于逻辑扇区)
    struct timeval now;
    gettimeofday(&now, NULL);

    double elapsed = (now.tv_sec - progress->start_time.tv_sec) +
                    (now.tv_usec - progress->start_time.tv_usec) / 1000000.0;

    if (elapsed > 0) {
        // sectors_per_second 是逻辑扇区/秒
        progress->sectors_per_second = progress->sectors_scanned / elapsed;

        // 估算剩余时间
        unsigned long remaining_sectors = progress->total_sectors - progress->sectors_scanned;
        if (progress->sectors_per_second > 0) {
            progress->estimated_remaining_sec = (int)(remaining_sectors / progress->sectors_per_second);
        }
    }

    // 更新百分比
    progress->progress_percent = (double)progress->sectors_scanned / progress->total_sectors * 100;

    // 智能更新显示频率（增加时间间隔控制）
    static unsigned long last_displayed_sector = 0;
    static double last_displayed_percent = 0.0;
    static struct timeval last_display_time = {0, 0};

    int should_update = 0;

    // 计算距离上次显示的时间间隔
    double time_since_last_display = 0.0;
    if (last_display_time.tv_sec > 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        time_since_last_display = (now.tv_sec - last_display_time.tv_sec) +
                                 (now.tv_usec - last_display_time.tv_usec) / 1000000.0;
    }

    // 条件 1：时间间隔超过 1 秒
    if (time_since_last_display >= 1.0) {
        should_update = 1;
    }

    // 条件 2：发现问题扇区（欠佳或更差）- 立即显示
    if (category >= TIME_CATEGORY_POOR) {
        should_update = 1;
    }

    // 条件 3：百分比变化超过 1%（降低频率）
    if (progress->progress_percent - last_displayed_percent >= 1.0) {
        should_update = 1;
    }

    // 条件 4：第一次更新或最后一个逻辑扇区
    if (last_displayed_sector == 0 || progress->sectors_scanned == progress->total_sectors) {
        should_update = 1;
    }

    if (should_update) {
        update_progress_display(progress, categories, geometry);
        last_displayed_sector = current_sector;
        last_displayed_percent = progress->progress_percent;
        gettimeofday(&last_display_time, NULL);  // 记录显示时间
    }
}

/**
 * 打印详细统计信息（用于实时显示）
 */
static void print_live_statistics(const TimeCategories* categories) {
    if (!categories) return;

    // 定义分类信息
    struct {
        TimeCategoryType type;
        const char* name;
        const char* description;
        int threshold;
        const char* operator;
    } category_info[] = {
        {TIME_CATEGORY_EXCELLENT, "优秀", "响应时间极佳", categories->excellent_max, "≤"},
        {TIME_CATEGORY_GOOD, "良好", "响应时间很好", categories->good_max, "≤"},
        {TIME_CATEGORY_NORMAL, "正常", "响应时间正常", categories->normal_max, "≤"},
        {TIME_CATEGORY_GENERAL, "一般", "响应时间开始变慢", categories->general_max, "≤"},
        {TIME_CATEGORY_POOR, "欠佳", "响应时间较差", categories->poor_max, "≤"},
        {TIME_CATEGORY_SEVERE, "严重", "响应时间很差", categories->severe_max, "≤"},
        {TIME_CATEGORY_SUSPECT, "可疑", "需要重测确认", categories->suspect_threshold, ">"},
        {TIME_CATEGORY_DAMAGED, "损坏", "真正的坏道", categories->severe_max, ">"}
    };

    for (int i = 0; i < 8; i++) {
        TimeCategoryType type = category_info[i].type;
        const char* name = category_info[i].name;
        const char* description = category_info[i].description;
        int threshold = category_info[i].threshold;
        const char* operator = category_info[i].operator;
        unsigned long count = categories->counts[type];

        double percentage = 0.0;
        if (categories->total_reads > 0) {
            percentage = (double)count / categories->total_reads * 100;
        }

        if (count > 0) {
            // 有数据时使用彩色显示
            printf("  %s%-8s\033[0m: %8lu 次 (%6.2f%%)   %s %4d ms (%s)\n",
                   get_category_color_str(type), name, count, percentage,
                   operator, threshold, description);
        } else {
            // 没有数据时使用暗色显示
            if (type == TIME_CATEGORY_DAMAGED) {
                printf("  \033[90m%-8s: %8lu 次 (%6.2f%%)   %s %4d ms 或发生 IO 错误 (%s)\033[0m\n",
                       name, count, percentage, operator, threshold, description);
            } else {
                printf("  \033[90m%-8s: %8lu 次 (%6.2f%%)   %s %4d ms (%s)\033[0m\n",
                       name, count, percentage, operator, threshold, description);
            }
        }
    }
}

/**
 * 打印完整的进度显示区域（多行布局）
 */
static void print_full_progress_display(const ScanProgress* progress, const TimeCategories* categories, const DeviceGeometry* geometry) {
    if (!progress) return;

    // 1. 简化的进度条（30字符宽度，控制总行宽在80列以内）
    int bar_width = 25;
    int filled = (int)(progress->progress_percent / 100.0 * bar_width);

    printf("\033[36m进度:\033[0m [");
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            printf("█");
        } else {
            printf("░");
        }
    }
    printf("] %5.1f%% ", progress->progress_percent);

    // 计算并显示字节速度 (基于逻辑扇区大小转换为字节/秒)
    if (progress->sectors_per_second > 0 && geometry) {
        double bytes_per_second = progress->sectors_per_second * geometry->sector_size;
        if (bytes_per_second >= 1024*1024*1024) {
            printf("%.1fG/s ", bytes_per_second / (1024*1024*1024));
        } else if (bytes_per_second >= 1024*1024) {
            printf("%.1fM/s ", bytes_per_second / (1024*1024));
        } else if (bytes_per_second >= 1024) {
            printf("%.1fK/s ", bytes_per_second / 1024);
        } else {
            printf("%.0fB/s ", bytes_per_second);
        }
    } else {
        printf("--.-/s ");
    }

    // 计算并显示经过时间
    struct timeval now;
    gettimeofday(&now, NULL);
    double elapsed = (now.tv_sec - progress->start_time.tv_sec) +
                    (now.tv_usec - progress->start_time.tv_usec) / 1000000.0;
    int elapsed_hours = (int)(elapsed / 3600);
    int elapsed_minutes = (int)((elapsed - elapsed_hours * 3600) / 60);
    int elapsed_seconds = (int)(elapsed - elapsed_hours * 3600 - elapsed_minutes * 60);

    printf("%02d:%02d:%02d ", elapsed_hours, elapsed_minutes, elapsed_seconds);

    // 显示剩余时间
    if (progress->estimated_remaining_sec > 0) {
        int hours = progress->estimated_remaining_sec / 3600;
        int minutes = (progress->estimated_remaining_sec % 3600) / 60;
        int seconds = progress->estimated_remaining_sec % 60;

        printf("剩余 %02d:%02d:%02d", hours, minutes, seconds);
    } else {
        printf("剩余 --:--:--");
    }

    printf("\n");

    // 2. 信息栏（空行）
    printf("\n");

    // 3. 分类统计（8行）
    printf("分类统计：\n");
    print_live_statistics(categories);
}

/**
 * 更新进度显示（多行版本 - 完全重绘方式，稳定可靠）
 */
static void update_progress_display(const ScanProgress* progress, const TimeCategories* categories, const DeviceGeometry* geometry) {
    if (!progress) return;

    static int first_display = 1;

    if (first_display) {
        // 第一次显示
        print_full_progress_display(progress, categories, geometry);
        first_display = 0;
    } else {
        // 后续更新：完全重绘整个区域
        // 1. 向上移动到进度条开始位置
        printf("\033[11A");
        // 2. 清除从当前位置到屏幕底部的所有内容
        printf("\033[J");
        // 3. 重新绘制整个进度显示区域
        print_full_progress_display(progress, categories, geometry);
    }

    fflush(stdout);
}

/**
 * 完成进度显示（换行到下一行）
 */
static void finish_progress_display(void) {
    printf("\n");  // 进度完成后换行

    fflush(stdout);
}

/**
 * 执行逻辑扇区读取
 * @param sector 逻辑扇区号
 * @param block_size 每次读取的字节数 (通常是逻辑扇区大小的倍数)
 */
static int perform_sector_read(int fd, unsigned long sector, size_t block_size,
                              char* buffer, DeviceGeometry* geometry) {
    // 计算字节偏移量 (逻辑扇区号 × 逻辑扇区大小)
    off_t offset = (off_t)sector * geometry->sector_size;

    // 定位到指定位置
    if (lseek(fd, offset, SEEK_SET) == -1) {
        return -1;
    }

    // 记录开始时间
    struct timeval start, end;
    gettimeofday(&start, NULL);

    // 执行读取
    ssize_t bytes_read = read(fd, buffer, block_size);

    // 记录结束时间
    gettimeofday(&end, NULL);

    // 计算耗时（毫秒）
    int time_ms = (end.tv_sec - start.tv_sec) * 1000 +
                  (end.tv_usec - start.tv_usec) / 1000;

    if (bytes_read != (ssize_t)block_size) {
        return -1;  // 读取失败
    }

    return time_ms;
}

/**
 * 处理可疑块
 */
static int handle_suspect_block(unsigned long sector,
                               const ScanOptions* opts, FILE* log_file,
                               const char* device_path) {
    if (opts->suspect_retries <= 0) {
        return -1;  // 不进行重测
    }

    // 配置重测参数
    RetestConfig retest_config;
    init_retest_config(&retest_config);

    // 从扫描选项配置重测参数
    set_retest_config(&retest_config, opts->suspect_retries, opts->suspect_interval);

    // 启用静默模式以避免干扰进度条
    set_retest_silent_mode(&retest_config, 1);

    // 执行重测
    RetestResult retest_result;
    int result = perform_sector_retest(sector, device_path, &retest_config, &retest_result);

    if (result == 0) {
        if (retest_result.final_category == TIME_CATEGORY_DAMAGED) {
            // 确认为坏道
            if (log_file) {
                log_sector_result(log_file, sector, -1, TIME_CATEGORY_DAMAGED, "重测确认坏道");
            }
            return -1;
        } else {
            // 重测通过，返回平均时间
            if (log_file) {
                char notes[128];
                snprintf(notes, sizeof(notes), "重测通过,平均%dms", retest_result.average_time);
                log_sector_result(log_file, sector, retest_result.average_time,
                                TIME_CATEGORY_NORMAL, notes);
            }
            return retest_result.average_time;
        }
    }

    // 重测失败
    if (log_file) {
        log_sector_result(log_file, sector, -1, TIME_CATEGORY_DAMAGED, "重测失败");
    }
    return -1;
}

/**
 * 记录扇区结果到日志文件
 */
static void log_sector_result(FILE* log_file, unsigned long sector, int read_time_ms,
                             TimeCategoryType category, const char* notes) {
    if (!log_file) return;

    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    const char* category_name = "未知";
    switch (category) {
        case TIME_CATEGORY_EXCELLENT: category_name = "优秀"; break;
        case TIME_CATEGORY_GOOD: category_name = "良好"; break;
        case TIME_CATEGORY_NORMAL: category_name = "正常"; break;
        case TIME_CATEGORY_GENERAL: category_name = "一般"; break;
        case TIME_CATEGORY_POOR: category_name = "欠佳"; break;
        case TIME_CATEGORY_SEVERE: category_name = "严重"; break;
        case TIME_CATEGORY_SUSPECT: category_name = "可疑"; break;
        case TIME_CATEGORY_DAMAGED: category_name = "损坏"; break;
        case TIME_CATEGORY_COUNT:
        default: category_name = "未知"; break;
    }

    fprintf(log_file, "%s,扇区_%lu,%d,%s,%s\n",
            timestamp, sector, read_time_ms, category_name, notes ? notes : "");
    fflush(log_file);
}

/**
 * 打印最终摘要
 */
static void print_final_summary(const ScanProgress* progress, const DeviceGeometry* geometry) {
    printf("\n");
    printf("\033[1;34m═══════════════════════════════════════════════════════════════\033[0m\n");
    printf("\033[1;34m                      扫描完成摘要                            \033[0m\n");
    printf("\033[1;34m═══════════════════════════════════════════════════════════════\033[0m\n");

    printf("\033[36m总扫描扇区数:\033[0m %lu\n", progress->sectors_scanned);
    printf("\033[36m扫描进度:\033[0m %.2f%%\n", progress->progress_percent);

    if (progress->sectors_per_second > 0) {
        printf("\033[36m平均扫描速度:\033[0m %.1f 逻辑扇区/秒 (%.1f MB/s)\n",
               progress->sectors_per_second,
               progress->sectors_per_second * geometry->sector_size / (1024*1024));
    }

    if (g_scan_interrupted) {
        printf("\033[33m扫描状态:\033[0m 用户中断\n");
    } else {
        printf("\033[32m扫描状态:\033[0m 正常完成\n");
    }

    printf("\033[1;34m═══════════════════════════════════════════════════════════════\033[0m\n");
}

/**
 * 主扫描函数
 */
int scan_device(const ScanOptions* opts) {
    if (!opts) {
        fprintf(stderr, "错误: 扫描选项为空\n");
        return -1;
    }

    // 设置信号处理
    setup_signal_handlers();

    // 收集设备信息
    DeviceInfo device_info;
    initialize_device_info(&device_info, opts->device);

    printf("\033[1;32m【设备检测】\033[m正在收集设备信息...\n");
    if (collect_device_info(&device_info) != 0) {
        printf("\033[33m【警告】\033[m设备信息收集不完整，继续扫描\n");
    }

    // 打印设备信息
    print_device_info(&device_info);

    // 获取设备几何信息
    DeviceGeometry geometry;
    if (get_device_geometry(opts->device, &geometry) != 0) {
        return -1;
    }

    // 解析扫描位置
    unsigned long start_sector, end_sector;
    if (parse_positions(opts, geometry.total_sectors, &start_sector, &end_sector) != 0) {
        return -1;
    }

    // 验证块大小
    if (opts->block_size % geometry.sector_size != 0) {
        fprintf(stderr, "错误: 块大小(%zu)必须是逻辑扇区大小(%u)的倍数\n",
                opts->block_size, geometry.sector_size);
        return -1;
    }

    // 设置可疑块阈值（如果为自动）
    int suspect_threshold = opts->suspect_threshold;
    if (suspect_threshold == DEFAULT_SUSPECT_THRESHOLD) {
        suspect_threshold = get_recommended_suspect_threshold(&device_info);
        printf("\033[36m【自动设置】\033[m根据设备类型设置可疑块阈值: %d 毫秒\n",
               suspect_threshold);
    }

    // 初始化时间分类
    TimeCategories categories;
    initialize_time_categories(&categories, device_info.device_type);

    // 如果有配置文件，加载自定义分类
    if (opts->config_file) {
        if (load_time_categories_config(&categories, opts->config_file) != 0) {
            printf("\033[33m【警告】\033[m配置文件加载失败，使用默认分类\n");
        }
    }

    // 验证时间分类设置
    if (validate_time_categories(&categories) != 0) {
        fprintf(stderr, "错误: 时间分类配置不合理\n");
        return -1;
    }

    // 计算采样参数
    unsigned long total_sectors = end_sector - start_sector;
    unsigned long sample_count;
    double step_size;

    if (opts->sample_ratio < 1.0) {
        sample_count = (unsigned long)(total_sectors * opts->sample_ratio);
        if (sample_count == 0) {
            sample_count = 1;  // 至少采样一个扇区
        }
        step_size = (double)total_sectors / sample_count;

        printf("\033[36m【采样模式】\033[m %.2f%% 采样 (%lu 个扇区，%s采样)\n",
               opts->sample_ratio * 100, sample_count,
               opts->random_sampling ? "随机" : "等间距");
        printf("\033[36m【采样间隔】\033[m %.1f 扇区\n", step_size);
    } else {
        sample_count = total_sectors;
        step_size = 1.0;
        printf("\033[36m【扫描模式】\033[m 全量扫描\n");
    }

    // 打印扫描头部信息
    print_scan_header(opts, start_sector, end_sector);

    // 打开设备进行扫描
    int fd = open_device_for_scan(opts->device);
    if (fd == -1) {
        return -1;
    }

    // 打开日志文件（如果需要）
    FILE* log_file = NULL;
    if (opts->log_filename) {
        log_file = fopen(opts->log_filename, "w");
        if (!log_file) {
            fprintf(stderr, "警告: 无法创建日志文件 %s: %s\n",
                    opts->log_filename, strerror(errno));
        } else {
            fprintf(log_file, "时间戳,扇区,读取时间(ms),分类,备注\n");
            printf("\033[32m【日志】\033[m日志文件已创建: %s\n", opts->log_filename);
        }
    }

    // 分配读取缓冲区
    char* buffer = aligned_alloc(4096, opts->block_size);
    if (!buffer) {
        fprintf(stderr, "错误: 内存分配失败\n");
        close(fd);
        if (log_file) fclose(log_file);
        return -1;
    }

    // 初始化进度跟踪
    ScanProgress progress = {0};
    progress.total_sectors = sample_count;
    gettimeofday(&progress.start_time, NULL);
    g_current_progress = &progress;

    printf("\n开始扫描...\n");

    // 初始化随机数种子（用于随机采样）
    if (opts->sample_ratio < 1.0 && opts->random_sampling) {
        srand(time(NULL));
    }

    // 主扫描循环
    for (unsigned long i = 0; i < sample_count && !g_scan_interrupted; i++) {
        unsigned long current_sector;

        if (opts->sample_ratio < 1.0) {
            // 采样模式：计算采样位置
            double base_position = i * step_size;

            if (opts->random_sampling) {
                // 随机采样：在固定间隔基础上添加随机偏移
                double max_offset = step_size * 0.8;  // 最大偏移为间隔的80%
                double random_offset = (rand() / (double)RAND_MAX) * max_offset - max_offset / 2;
                current_sector = start_sector + (unsigned long)(base_position + random_offset);

                // 确保不超出范围
                if (current_sector < start_sector) current_sector = start_sector;
                if (current_sector >= end_sector) current_sector = end_sector - 1;
            } else {
                // 等间距采样
                current_sector = start_sector + (unsigned long)base_position;
            }
        } else {
            // 全量扫描
            current_sector = start_sector + i;
        }

        // 执行逻辑扇区读取
        int read_time = perform_sector_read(fd, current_sector, opts->block_size,
                                          buffer, &geometry);

        TimeCategoryType category;

        if (read_time >= 0) {
            // 读取成功，分类时间
            category = categorize_time(&categories, read_time);

            // 检查是否为可疑块
            if (read_time >= suspect_threshold) {
                // 处理可疑块
                int retest_time = handle_suspect_block(current_sector, opts, log_file, opts->device);
                if (retest_time >= 0) {
                    read_time = retest_time;
                    category = categorize_time(&categories, read_time);
                }
            }

            // 记录到日志（如果满足阈值条件）
            if (log_file && (opts->log_threshold == 0 || read_time >= opts->log_threshold)) {
                log_sector_result(log_file, current_sector, read_time, category, NULL);
            }
        } else {
            // 读取失败
            category = TIME_CATEGORY_DAMAGED;
            categorize_time(&categories, 30000);  // 记录为极长时间

            printf("\n\033[31m【读取错误】\033[m扇区 %lu 读取失败: %s\n",
                   current_sector, strerror(errno));

            if (log_file) {
                log_sector_result(log_file, current_sector, -1, category, "读取失败");
            }
        }

        // 更新进度
        update_progress(&progress, current_sector, read_time, category, &categories, &geometry);

        // 等待延迟（如果设置了等待因子）
        if (opts->wait_factor > 0 && read_time > 0) {
            usleep(read_time * opts->wait_factor * 1000);
        }
    }

    // 清理资源
    free(buffer);
    close(fd);
    if (log_file) fclose(log_file);

    // 完成进度显示，将光标移到末尾
    finish_progress_display();
    print_final_summary(&progress, &geometry);

    g_current_progress = NULL;

    return g_scan_interrupted ? 1 : 0;
}
