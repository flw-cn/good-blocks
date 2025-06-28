// src/scanner.c - 磁盘扫描核心功能
#include "scanner.h"
#include "device_info/device_info.h"
#include "time_categories.h"
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
                           int read_time_ms, TimeCategoryType category);
static void print_progress_line(const ScanProgress* progress);
static int perform_sector_read(int fd, unsigned long sector, size_t block_size,
                              char* buffer, DeviceGeometry* geometry);
static int handle_suspect_block(int fd, unsigned long sector, size_t block_size,
                               char* buffer, DeviceGeometry* geometry,
                               const ScanOptions* opts, FILE* log_file);
static void generate_sample_positions(unsigned long start_sector, unsigned long end_sector,
                                     double sample_ratio, int random_sampling,
                                     unsigned long** positions, unsigned long* count);
static void print_final_summary(const ScanProgress* progress, const TimeCategories* categories);
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

    // 获取逻辑扇区大小
    geometry->sector_size = 512;
    if (ioctl(fd, BLKSSZGET, &geometry->sector_size) == -1) {
        fprintf(stderr, "警告: 无法获取逻辑扇区大小，使用默认值 %d\n", geometry->sector_size);
    }

    // 获取物理扇区大小
    unsigned int physical_sector_size = 512;
    if (ioctl(fd, BLKPBSZGET, &physical_sector_size) == -1) {
        fprintf(stderr, "警告: 无法获取物理扇区大小，假设与逻辑扇区大小相同\n");
        physical_sector_size = geometry->sector_size;
    }

    printf("\033[35m【设备几何】\033[m逻辑扇区大小: %d 字节\n", geometry->sector_size);
    printf("\033[35m【设备几何】\033[m物理扇区大小: %d 字节\n", physical_sector_size);

    // 获取总扇区数 (以逻辑扇区为单位)
    geometry->total_sectors = 0;
    if (ioctl(fd, BLKGETSIZE, &geometry->total_sectors) == -1) {
        fprintf(stderr, "错误: 无法获取总扇区数: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    // BLKGETSIZE 返回的扇区数总是以 512 字节为基准
    double total_gb = (double)geometry->total_sectors * 512 / (1024*1024*1024);
    printf("\033[35m【设备几何】\033[m设备总扇区数: %lu (以512字节计，%.2f GB)\n",
           geometry->total_sectors, total_gb);

    // 如果逻辑扇区大小不是 512，显示换算后的扇区数
    if (geometry->sector_size != 512) {
        unsigned long logical_sectors = geometry->total_sectors * 512 / geometry->sector_size;
        printf("\033[35m【设备几何】\033[m逻辑扇区数: %lu (以%d字节计)\n",
               logical_sectors, geometry->sector_size);
    }

    close(fd);
    return 0;
}

/**
 * 为扫描打开设备
 */
static int open_device_for_scan(const char* device_path) {
    int fd = open(device_path, O_RDONLY | O_DIRECT);
    if (fd == -1) {
        // 如果 O_DIRECT 失败，尝试不使用 O_DIRECT
        printf("\033[33m【设备打开】\033[m O_DIRECT 失败，尝试常规模式...\n");
        fd = open(device_path, O_RDONLY);
        if (fd == -1) {
            fprintf(stderr, "错误: 无法打开设备 %s: %s\n", device_path, strerror(errno));
            return -1;
        }
    } else {
        printf("\033[32m【设备打开】\033[m使用 O_DIRECT 模式打开设备\n");
    }

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
    printf("\033[36m扫描范围:\033[0m 扇区 %lu - %lu\n", start_sector, end_sector);
    printf("\033[36m扫描扇区数:\033[0m %lu\n", end_sector - start_sector);
    printf("\033[36m块大小:\033[0m %ld 字节\n", opts->block_size);

    if (opts->sample_ratio < 1.0) {
        unsigned long total_range = end_sector - start_sector;
        unsigned long sample_count = (unsigned long)(total_range * opts->sample_ratio);
        printf("\033[36m采样模式:\033[0m %.2f%% (%lu 个扇区，%s)\n",
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

    printf("\033[1;34m═══════════════════════════════════════════════════════════════\033[0m\n\n");
}

/**
 * 更新扫描进度
 */
static void update_progress(ScanProgress* progress, unsigned long current_sector,
                           int read_time_ms, TimeCategoryType category) {
    if (!progress) return;

    progress->current_sector = current_sector;
    progress->sectors_scanned++;
    progress->last_read_time = read_time_ms;
    progress->last_category = category;

    // 更新速度统计
    struct timeval now;
    gettimeofday(&now, NULL);

    double elapsed = (now.tv_sec - progress->start_time.tv_sec) +
                    (now.tv_usec - progress->start_time.tv_usec) / 1000000.0;

    if (elapsed > 0) {
        progress->sectors_per_second = progress->sectors_scanned / elapsed;

        // 估算剩余时间
        unsigned long remaining_sectors = progress->total_sectors - progress->sectors_scanned;
        if (progress->sectors_per_second > 0) {
            progress->estimated_remaining_sec = (int)(remaining_sectors / progress->sectors_per_second);
        }
    }

    // 更新百分比
    progress->progress_percent = (double)progress->sectors_scanned / progress->total_sectors * 100;

    // 每100个扇区更新一次显示（避免过于频繁）
    if (progress->sectors_scanned % 100 == 0 ||
        category == TIME_CATEGORY_POOR || category == TIME_CATEGORY_SEVERE) {
        print_progress_line(progress);
    }
}

/**
 * 打印进度行
 */
static void print_progress_line(const ScanProgress* progress) {
    if (!progress) return;

    // 清除当前行
    printf("\r\033[K");

    // 打印进度条
    int bar_width = 30;
    int filled = (int)(progress->progress_percent / 100.0 * bar_width);

    printf("\033[36m进度:\033[0m [");
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            printf("█");
        } else {
            printf("░");
        }
    }
    printf("] %6.2f%% ", progress->progress_percent);

    // 打印当前扇区和最后读取时间
    printf("扇区:%lu ", progress->current_sector);

    // 用颜色显示最后读取时间
    print_time_category(progress->last_category, progress->last_read_time);

    // 打印速度信息
    if (progress->sectors_per_second > 0) {
        printf(" %.1f扇区/秒", progress->sectors_per_second);

        if (progress->estimated_remaining_sec > 0) {
            int hours = progress->estimated_remaining_sec / 3600;
            int minutes = (progress->estimated_remaining_sec % 3600) / 60;
            int seconds = progress->estimated_remaining_sec % 60;

            if (hours > 0) {
                printf(" 剩余:%dh%dm", hours, minutes);
            } else if (minutes > 0) {
                printf(" 剩余:%dm%ds", minutes, seconds);
            } else {
                printf(" 剩余:%ds", seconds);
            }
        }
    }

    fflush(stdout);
}

/**
 * 执行扇区读取
 */
static int perform_sector_read(int fd, unsigned long sector, size_t block_size,
                              char* buffer, DeviceGeometry* geometry) {
    // 计算字节偏移量
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
static int handle_suspect_block(int fd, unsigned long sector, size_t block_size,
                               char* buffer, DeviceGeometry* geometry,
                               const ScanOptions* opts, FILE* log_file) {
    if (opts->suspect_retries <= 0) {
        return -1;  // 不进行重测
    }

    printf("\n\033[33m【可疑块检测】\033[m扇区 %lu 读取时间异常，开始重测...\n", sector);

    int success_count = 0;
    int total_time = 0;

    for (int retry = 0; retry < opts->suspect_retries; retry++) {
        // 等待间隔
        if (opts->suspect_interval > 0 && retry > 0) {
            usleep(opts->suspect_interval * 1000);
        }

        int read_time = perform_sector_read(fd, sector, block_size, buffer, geometry);

        if (read_time >= 0) {
            success_count++;
            total_time += read_time;

            printf("\033[33m【重测 %d/%d】\033[m扇区 %lu: %d ms\n",
                   retry + 1, opts->suspect_retries, sector, read_time);

            // 记录重测结果到日志
            if (log_file) {
                char notes[128];
                snprintf(notes, sizeof(notes), "重测%d/%d", retry + 1, opts->suspect_retries);
                log_sector_result(log_file, sector, read_time,
                                TIME_CATEGORY_NORMAL, notes);
            }
        } else {
            printf("\033[31m【重测 %d/%d】\033[m扇区 %lu: 读取失败\n",
                   retry + 1, opts->suspect_retries, sector);

            if (log_file) {
                char notes[128];
                snprintf(notes, sizeof(notes), "重测%d/%d失败", retry + 1, opts->suspect_retries);
                log_sector_result(log_file, sector, -1, TIME_CATEGORY_DAMAGED, notes);
            }
        }
    }

    if (success_count > 0) {
        int avg_time = total_time / success_count;
        printf("\033[32m【重测结果】\033[m扇区 %lu: 成功 %d/%d 次，平均时间 %d ms\n",
               sector, success_count, opts->suspect_retries, avg_time);
        return avg_time;
    } else {
        printf("\033[31m【重测结果】\033[m扇区 %lu: 全部重测失败，可能存在坏块\n", sector);
        return -1;
    }
}

/**
 * 生成采样位置
 */
static void generate_sample_positions(unsigned long start_sector, unsigned long end_sector,
                                     double sample_ratio, int random_sampling,
                                     unsigned long** positions, unsigned long* count) {
    unsigned long total_sectors = end_sector - start_sector;
    *count = (unsigned long)(total_sectors * sample_ratio);

    if (*count == 0) {
        *count = 1;  // 至少采样一个扇区
    }

    *positions = malloc(*count * sizeof(unsigned long));
    if (!*positions) {
        fprintf(stderr, "错误: 内存分配失败\n");
        *count = 0;
        return;
    }

    if (random_sampling) {
        // 随机采样
        srand(time(NULL));
        for (unsigned long i = 0; i < *count; i++) {
            (*positions)[i] = start_sector + (rand() % total_sectors);
        }

        // 排序以提高访问效率
        qsort(*positions, *count, sizeof(unsigned long),
              (int(*)(const void*, const void*))strcmp);

        printf("\033[36m【采样生成】\033[m生成 %lu 个随机采样位置\n", *count);
    } else {
        // 等间距采样
        double step = (double)total_sectors / *count;
        for (unsigned long i = 0; i < *count; i++) {
            (*positions)[i] = start_sector + (unsigned long)(i * step);
        }

        printf("\033[36m【采样生成】\033[m生成 %lu 个等间距采样位置 (间隔: %.1f)\n",
               *count, step);
    }
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
static void print_final_summary(const ScanProgress* progress, const TimeCategories* categories) {
    printf("\n\n\033[1;34m═══════════════════════════════════════════════════════════════\033[0m\n");
    printf("\033[1;34m                      扫描完成摘要                            \033[0m\n");
    printf("\033[1;34m═══════════════════════════════════════════════════════════════\033[0m\n");

    printf("\033[36m总扫描扇区数:\033[0m %lu\n", progress->sectors_scanned);
    printf("\033[36m扫描进度:\033[0m %.2f%%\n", progress->progress_percent);

    if (progress->sectors_per_second > 0) {
        printf("\033[36m平均扫描速度:\033[0m %.1f 扇区/秒\n", progress->sectors_per_second);
    }

    if (g_scan_interrupted) {
        printf("\033[33m扫描状态:\033[0m 用户中断\n");
    } else {
        printf("\033[32m扫描状态:\033[0m 正常完成\n");
    }

    printf("\n");

    // 打印时间分类统计
    print_time_statistics(categories);
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

    // 生成采样位置（如果需要）
    unsigned long* sample_positions = NULL;
    unsigned long sample_count = 0;

    if (opts->sample_ratio < 1.0) {
        generate_sample_positions(start_sector, end_sector, opts->sample_ratio,
                                 opts->random_sampling, &sample_positions, &sample_count);
        if (sample_count == 0) {
            fprintf(stderr, "错误: 采样位置生成失败\n");
            return -1;
        }
    } else {
        sample_count = end_sector - start_sector;
    }

    // 打印扫描头部信息
    print_scan_header(opts, start_sector, end_sector);

    // 打开设备进行扫描
    int fd = open_device_for_scan(opts->device);
    if (fd == -1) {
        if (sample_positions) free(sample_positions);
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
        if (sample_positions) free(sample_positions);
        return -1;
    }

    // 初始化进度跟踪
    ScanProgress progress = {0};
    progress.total_sectors = sample_count;
    gettimeofday(&progress.start_time, NULL);
    g_current_progress = &progress;

    printf("\n开始扫描...\n");

    // 主扫描循环
    for (unsigned long i = 0; i < sample_count && !g_scan_interrupted; i++) {
        unsigned long current_sector;

        if (sample_positions) {
            current_sector = sample_positions[i];
        } else {
            current_sector = start_sector + i;
        }

        // 执行扇区读取
        int read_time = perform_sector_read(fd, current_sector, opts->block_size,
                                          buffer, &geometry);

        TimeCategoryType category;

        if (read_time >= 0) {
            // 读取成功，分类时间
            category = categorize_time(&categories, read_time);

            // 检查是否为可疑块
            if (read_time >= suspect_threshold) {
                // 处理可疑块
                int retest_time = handle_suspect_block(fd, current_sector, opts->block_size,
                                                     buffer, &geometry, opts, log_file);
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
        update_progress(&progress, current_sector, read_time, category);

        // 等待延迟（如果设置了等待因子）
        if (opts->wait_factor > 0 && read_time > 0) {
            usleep(read_time * opts->wait_factor * 1000);
        }
    }

    // 清理资源
    free(buffer);
    close(fd);
    if (log_file) fclose(log_file);
    if (sample_positions) free(sample_positions);

    // 打印最终摘要
    printf("\n");  // 换行，避免进度条影响
    print_final_summary(&progress, &categories);

    g_current_progress = NULL;

    return g_scan_interrupted ? 1 : 0;
}
