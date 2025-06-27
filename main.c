#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <ctype.h>

#define BLOCK_SIZE_DEFAULT          512
#define MAX_CATEGORIES              20
#define MIN_REPORT_INTERVAL         1000
#define DEFAULT_SUSPECT_THRESHOLD   100
#define DEFAULT_SUSPECT_RETRIES     10
#define DEFAULT_SUSPECT_INTERVAL    100

typedef struct {
    unsigned long   block_num;
    int             is_suspect;
    long            final_elapsed;
} BlockResult;

typedef struct {
    char    name[20];
    long    max_time;
    char    color[20];
    long    count;
} TimeCategory;

// 从文件加载时间分类
int load_categories(const char *filename, TimeCategory *cats) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        fprintf(stderr, "警告: 无法打开配置文件 '%s'，使用默认分类\n", filename);
        return -1;
    }

    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), file) && count < MAX_CATEGORIES) {
        // 跳过空行和注释
        if (line[0] == '#' || line[0] == '\n') continue;

        char name[20] = {0};
        char color[20] = {0};
        long max_time = 0;

        // 解析行: 名称,时间,颜色
        if (sscanf(line, "%19[^,],%ld,%19s", name, &max_time, color) == 3) {
            strncpy(cats[count].name, name, sizeof(cats[count].name));
            cats[count].max_time = max_time;
            strncpy(cats[count].color, color, sizeof(cats[count].color));
            count++;
        }
    }

    fclose(file);

    if (count == 0) {
        fprintf(stderr, "错误: 配置文件 '%s' 未包含有效分类，使用默认分类\n", filename);
        return -1;
    }

    // 确保最后一个分类是坏道（max_time=0）
    if (count > 0 && cats[count-1].max_time != 0) {
        fprintf(stderr, "警告: 最后一个分类应设为坏道（max_time=0），自动添加\n");
        strcpy(cats[count].name, "坏道");
        cats[count].max_time = 0;
        strcpy(cats[count].color, "\033[1;31m");
        count++;
    }

    return count;
}

// 打印时间分类定义
void print_category_definitions(TimeCategory *cats, int count) {
    printf("\033[33m【准备扫描】\033[m时间分类定义:\n");
    for (int i = 0; i < count; i++) {
        if (i < count - 2) {
            printf("\033[33m【准备扫描】\033[m  %s%s\033[0m: ≤ %4ld ms\n", cats[i].color, cats[i].name, cats[i].max_time);
        } else {
            printf("\033[33m【准备扫描】\033[m  %s%s\033[0m: > %4ld ms\n", cats[i].color, cats[i].name, cats[i].max_time);
        }
    }
}

// 日志记录函数
void log_block(FILE *logfile, unsigned long block, unsigned long sector_offset,
               size_t block_size, int sectors_per_block,
               long elapsed, const char *status) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

    unsigned long start_sector = block * sectors_per_block + sector_offset;

    // 只记录第一个扇区，添加块大小信息
    fprintf(logfile, "%lu # %s # %ld ms # %s # %d sectors\n",
            start_sector, timestamp, elapsed, status, sectors_per_block);

    fflush(logfile);
}

// 打印进度报告
void print_progress_report(unsigned long current, unsigned long total,
                           TimeCategory *cats, int cat_count,
                           struct timespec *start_time) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    double progress = (total > 0) ? 100.0 * current / total : 0.0;
    double elapsed_sec = (now.tv_sec - start_time->tv_sec) +
                        (now.tv_nsec - start_time->tv_nsec) / 1000000000.0;
    double speed = (elapsed_sec > 0) ? current / elapsed_sec : 0;
    double remaining_sec = (speed > 0 && current < total) ? (total - current) / speed : 0;

    printf("\r进度: %5.1f%% | 速度: %5.0f 块/秒 | 剩余: %dh%02dm%02ds | ",
           progress, speed,
           (int)(remaining_sec / 3600), (int)(remaining_sec) % 3600 / 60, (int)remaining_sec % 60);

    for (int i = 0; i < cat_count; i++) {
        if (cats[i].count > 0) {
            printf("%s%s\033[0m: %lu ", cats[i].color, cats[i].name, cats[i].count);
        } else {
            printf("%s: %lu ", cats[i].name, cats[i].count);
        }
    }

    fflush(stdout);
}

// 解析百分比参数
unsigned long parse_percentage(const char *str, unsigned long total_sectors) {
    char *endptr;
    double percent = strtod(str, &endptr);

    if (*endptr == '%' && percent >= 0 && percent <= 100) {
        return (unsigned long)(percent / 100.0 * total_sectors);
    }

    fprintf(stderr, "错误: 无效的百分比参数 '%s'\n", str);

    exit(1);
}

// 采样迭代器
typedef struct {
    unsigned long   total_blocks;
    unsigned long   current_index;
    unsigned long   total_samples;
    double          sample_ratio;
    int             random_sampling;    // 0=均匀采样, 1=随机采样
    double          step;               // 均匀采样步长
    unsigned long   last_block;         // 上次返回的块号
} SampleIterator;

// 初始化采样迭代器
int init_sample_iterator(SampleIterator *iter, unsigned long total_blocks,
                        double sample_ratio, int random_sampling) {
    iter->total_blocks = total_blocks;
    iter->current_index = 0;
    iter->sample_ratio = sample_ratio;
    iter->random_sampling = random_sampling;
    iter->last_block = 0;

    iter->total_samples = (unsigned long)(total_blocks * sample_ratio / 100.0);
    if (iter->total_samples == 0) iter->total_samples = 1;
    if (iter->total_samples > total_blocks) iter->total_samples = total_blocks;

    if (random_sampling) {
        srand(time(NULL)); // 初始化随机数种子
    } else {
        iter->step = (double)total_blocks / iter->total_samples;
    }

    return 0;
}

// 获取下一个采样块号，返回-1表示结束
long get_next_sample_block(SampleIterator *iter) {
    if (iter->current_index >= iter->total_samples) {
        return -1; // 采样结束
    }

    unsigned long block_num;

    if (iter->random_sampling) {
        // 随机采样：在剩余空间中随机选择
        unsigned long remaining_blocks = iter->total_blocks - iter->last_block;
        unsigned long remaining_samples = iter->total_samples - iter->current_index;

        if (remaining_samples >= remaining_blocks) {
            block_num = iter->last_block;
        } else {
            // 计算平均间隔，然后在间隔内随机选择
            double avg_gap = (double)remaining_blocks / remaining_samples;
            unsigned long max_gap = (unsigned long)(avg_gap * 2);
            if (max_gap < 1) max_gap = 1;
            unsigned long gap = 1 + (rand() % max_gap);
            block_num = iter->last_block + gap;
            if (block_num >= iter->total_blocks) {
                block_num = iter->total_blocks - 1;
            }
        }
    } else {
        // 均匀采样
        block_num = (unsigned long)(iter->current_index * iter->step);
        if (block_num >= iter->total_blocks) {
            block_num = iter->total_blocks - 1;
        }
    }

    iter->last_block = block_num;
    iter->current_index++;

    return (long)block_num;
}

// 生成抽样块列表
unsigned long* generate_sample_blocks(unsigned long total_blocks, double sample_ratio, unsigned long *sample_count) {
    *sample_count = (unsigned long)(total_blocks * sample_ratio / 100.0);
    if (*sample_count == 0) *sample_count = 1;
    if (*sample_count > total_blocks) *sample_count = total_blocks;

    unsigned long *sample_blocks = malloc(*sample_count * sizeof(unsigned long));
    if (!sample_blocks) return NULL;

    if (sample_ratio >= 100.0) {
        // 100%采样，顺序扫描
        for (unsigned long i = 0; i < *sample_count; i++) {
            sample_blocks[i] = i;
        }
    } else {
        // 按比例均匀采样
        double step = (double)total_blocks / *sample_count;
        for (unsigned long i = 0; i < *sample_count; i++) {
            sample_blocks[i] = (unsigned long)(i * step);
        }
    }

    return sample_blocks;
}

// 可疑块重测函数
long retest_suspect_block(int fd, void *buffer, size_t block_size,
                          unsigned long block_num, int sectors_per_block,
                          unsigned long sector_offset, int sector_size,
                          int retries, int interval_ms) {
    if (retries < 3) retries = 3; // 至少需要3次才能去掉最大最小值

    long *results = malloc(retries * sizeof(long));
    if (!results) return -1;

    struct timespec start, end;
    off_t block_offset = (off_t)(block_num * sectors_per_block + sector_offset) * sector_size;

    int valid_count = 0;
    for (int i = 0; i < retries; i++) {
        // 强制停顿
        if (interval_ms > 0) {
            struct timespec sleep_time = {
                .tv_sec = interval_ms / 1000,
                .tv_nsec = (interval_ms % 1000) * 1000000
            };
            nanosleep(&sleep_time, NULL);
        }

        // 重新定位
        if (lseek(fd, block_offset, SEEK_SET) != block_offset) {
            continue;
        }

        clock_gettime(CLOCK_MONOTONIC, &start);
        ssize_t bytes_read = read(fd, buffer, block_size);
        clock_gettime(CLOCK_MONOTONIC, &end);

        if (bytes_read != (ssize_t)block_size) {
            free(results);
            return -1; // 读取失败
        }

        long elapsed = (end.tv_sec - start.tv_sec) * 1000 +
                      (end.tv_nsec - start.tv_nsec) / 1000000;

        results[valid_count++] = elapsed;
    }

    if (valid_count < 3) {
        free(results);
        return -1; // 重测失败次数太多
    }

    // 找出最大值和最小值，无需排序
    long min_val = results[0];
    long max_val = results[0];
    long sum = 0;

    for (int i = 1; i < valid_count; i++) {
        if (results[i] < min_val) min_val = results[i];
        if (results[i] > max_val) max_val = results[i];
        sum += results[i];
    }

    // 如果样本数大于2，去掉最大最小值
    if (valid_count > 2) {
        sum = sum - min_val - max_val;
        valid_count -= 2;
    }

    free(results);
    return sum / valid_count;
}

typedef struct {
    const char *device;
    const char *start_str;
    const char *end_str;
    size_t      block_size;
    const char *log_filename;
    int         log_threshold;
    const char *config_file;
    double      sample_ratio;
    int         random_sampling;
    int         wait_factor;
    int         suspect_threshold;
    int         suspect_retries;
    int         suspect_interval;
} ScanOptions;

// 解析命令行参数
int parse_arguments(int argc, char *argv[], ScanOptions *opts) {
    // 初始化默认值
    opts->device            = NULL;
    opts->start_str         = NULL;
    opts->end_str           = NULL;
    opts->block_size        = BLOCK_SIZE_DEFAULT;
    opts->log_filename      = NULL;
    opts->log_threshold     = 100;      // 默认日志阈值为 100ms
    opts->config_file       = NULL;
    opts->sample_ratio      = 100.0;    // 默认 100% 采样
    opts->random_sampling   = 0;        // 默认均匀采样
    opts->wait_factor       = 0;        // 默认不等待
    opts->suspect_threshold = DEFAULT_SUSPECT_THRESHOLD;
    opts->suspect_retries   = DEFAULT_SUSPECT_RETRIES;
    opts->suspect_interval  = DEFAULT_SUSPECT_INTERVAL;

    if (argc < 4) {
        fprintf(stderr, "用法: %s <设备> <起始扇区> <结束扇区> [选项]\n", argv[0]);
        fprintf(stderr, "选项:\n");
        fprintf(stderr, "  -b <块大小>     块大小（字节数，默认 512）\n");
        fprintf(stderr, "  -l <日志文件>   日志文件\n");
        fprintf(stderr, "  -L <日志阈值>   记录到日志的阈值，默认为 100ms\n");
        fprintf(stderr, "  -c <配置文件>   时间分类配置文件\n");
        fprintf(stderr, "  -s <百分比>     抽样检查百分比（如 10 表示 10%%，默认 100%%）\n");
        fprintf(stderr, "  -r              启用随机采样（默认均匀采样）\n");
        fprintf(stderr, "  -w <因子>       等待时间因子（如 200 表示 200%%，默认 0 不等待）\n");
        fprintf(stderr, "  -S <阈值>       可疑块阈值（ms，默认 100）\n");
        fprintf(stderr, "  -R <次数>       可疑块重测次数（默认 10）\n");
        fprintf(stderr, "  -I <间隔>       可疑块重测间隔（ms，默认 100）\n");
        fprintf(stderr, "  --no-auto       禁用自动设备检测和配置\n");
        fprintf(stderr, "\n示例:\n");
        fprintf(stderr, "  %s /dev/sda 0 1000000\n", argv[0]);
        fprintf(stderr, "  %s /dev/sda \"97%%\" \"100%%\" -b 4096 -l scan.log -s 50\n", argv[0]);
        fprintf(stderr, "  %s /dev/sda 0 1000000 -c categories.conf -S 200 -R 5\n", argv[0]);
        return 1;
    }

    int positional_args = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            opts->block_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            opts->log_filename = argv[++i];
        } else if (strcmp(argv[i], "-L") == 0 && i + 1 < argc) {
            opts->log_threshold = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            opts->config_file = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            opts->sample_ratio = atof(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0) {
            opts->random_sampling = 1;
        } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            opts->wait_factor = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-S") == 0 && i + 1 < argc) {
            opts->suspect_threshold = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-R") == 0 && i + 1 < argc) {
            opts->suspect_retries = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            opts->suspect_interval = atoi(argv[++i]);
        } else {
            if (positional_args == 0) {
                opts->device = argv[i];
            } else if (positional_args == 1) {
                opts->start_str = argv[i];
            } else if (positional_args == 2) {
                opts->end_str = argv[i];
            }
            positional_args++;
        }
    }

    if (!opts->device || !opts->start_str || !opts->end_str) {
        fprintf(stderr, "错误: 缺少必需参数: 设备, 起始扇区, 结束扇区\n");
        return 1;
    }

    // 在这里打印命令行参数信息
    printf("\033[36m【参数信息】\033[m设备: %s\n", opts->device);
    printf("\033[36m【参数信息】\033[m测试范围: %s - %s\n", opts->start_str, opts->end_str);
    printf("\033[36m【参数信息】\033[m块大小: %zu 字节\n", opts->block_size);
    if (opts->log_filename) {
        printf("\033[36m【参数信息】\033[m日志文件: %s\n", opts->log_filename);
        printf("\033[36m【参数信息】\033[m日志阈值: %d ms\n", opts->log_threshold);
    } else {
        printf("\033[36m【参数信息】\033[m日志文件: 无\n");
    }
    if (opts->config_file) {
        printf("\033[36m【参数信息】\033[m配置文件: %s\n", opts->config_file);
    } else {
        printf("\033[36m【参数信息】\033[m配置文件: 无（采用程序预置配置）\n");
    }
    printf("\033[36m【参数信息】\033[m抽样比例: %.2f%%\n", opts->sample_ratio);
    printf("\033[36m【参数信息】\033[m随机采样: %s\n", opts->random_sampling ? "启用" : "禁用");
    printf("\033[36m【参数信息】\033[m等待时间因子: %d\n", opts->wait_factor);
    printf("\033[36m【参数信息】\033[m可疑块阈值: %d ms\n", opts->suspect_threshold);
    printf("\033[36m【参数信息】\033[m可疑块重测次数: %d\n", opts->suspect_retries);
    printf("\033[36m【参数信息】\033[m可疑块重测间隔: %d ms\n", opts->suspect_interval);

    return 0;
}

typedef struct {
    char device_type[32];    // "SSD", "HDD", "NVMe", "Unknown"
    int is_rotational;       // 1=机械硬盘, 0=固态硬盘
    int rpm;                 // 转速，0表示SSD或未知
    char model[64];          // 设备型号
    char vendor[32];         // 厂商
} DeviceTypeInfo;

// 检测设备类型
int detect_device_type(const char *device_path, DeviceTypeInfo *info) {
    char sys_path[256];
    char buffer[256];
    FILE *file;

    printf("\033[1;94m【设备类型】\033[m正在检测设备类型...\n");

    // 初始化
    strcpy(info->device_type, "Unknown");
    info->is_rotational = -1;
    info->rpm = 0;
    strcpy(info->model, "Unknown");
    strcpy(info->vendor, "Unknown");

    // 从设备路径提取设备名 (例如: /dev/sda -> sda)
    const char *dev_name = strrchr(device_path, '/');
    if (!dev_name) return -1;
    dev_name++; // 跳过 '/'

    // 对于分区，需要获取主设备名
    char main_dev[32];
    strcpy(main_dev, dev_name);

    // 处理不同类型的设备名
    if (strncmp(main_dev, "nvme", 4) == 0) {
        // NVMe设备: nvme0n1p1 -> nvme0n1
        char *p_pos = strstr(main_dev, "p");
        if (p_pos && isdigit(*(p_pos + 1))) {
            *p_pos = '\0';  // 截断到p之前
        }
        strcpy(info->device_type, "NVMe");
        info->is_rotational = 0;
        info->rpm = 0;
    } else {
        // 传统设备: sda1 -> sda, mmcblk0p1 -> mmcblk0
        if (strncmp(main_dev, "mmcblk", 6) == 0) {
            // eMMC/SD卡设备: mmcblk0p1 -> mmcblk0
            char *p_pos = strstr(main_dev, "p");
            if (p_pos && isdigit(*(p_pos + 1))) {
                *p_pos = '\0';
            }
        } else {
            // SATA/SCSI设备: sda1 -> sda
            for (int i = strlen(main_dev) - 1; i >= 0; i--) {
                if (isdigit(main_dev[i])) {
                    main_dev[i] = '\0';
                } else {
                    break;
                }
            }
        }
    }

    // 检查rotational属性（对于非NVMe设备）
    if (strcmp(info->device_type, "NVMe") != 0) {
        snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/queue/rotational", main_dev);
        file = fopen(sys_path, "r");
        if (file) {

            if (fgets(buffer, sizeof(buffer), file)) {
                info->is_rotational = atoi(buffer);
                if (info->is_rotational == 0 && strcmp(info->device_type, "Unknown") == 0) {
                    strcpy(info->device_type, "SSD");
                } else if (info->is_rotational == 1) {
                    strcpy(info->device_type, "HDD");
                }
            }
            fclose(file);
        }
    }

    // 尝试获取设备型号
    snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/device/model", main_dev);
    file = fopen(sys_path, "r");
    if (file) {
        if (fgets(buffer, sizeof(buffer), file)) {
            // 移除换行符和空格
            buffer[strcspn(buffer, "\n\r")] = '\0';
            // 移除前后空格
            char *start = buffer;
            while (isspace(*start)) start++;
            char *end = start + strlen(start) - 1;
            while (end > start && isspace(*end)) *end-- = '\0';
            if (strlen(start) > 0) {
                strncpy(info->model, start, sizeof(info->model) - 1);
            }
        }
        fclose(file);
    }

    // 尝试获取厂商信息
    snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/device/vendor", main_dev);
    file = fopen(sys_path, "r");
    if (file) {
        if (fgets(buffer, sizeof(buffer), file)) {
            buffer[strcspn(buffer, "\n\r")] = '\0';
            char *start = buffer;
            while (isspace(*start)) start++;
            char *end = start + strlen(start) - 1;
            while (end > start && isspace(*end)) *end-- = '\0';
            if (strlen(start) > 0) {
                strncpy(info->vendor, start, sizeof(info->vendor) - 1);
            }
        }
        fclose(file);
    }

    printf("\033[1;94m【设备类型】\033[m设备类型检测结果:\n");
    printf("\033[1;94m【设备类型】\033[m  类型: %s\n", info->device_type);
    printf("\033[1;94m【设备类型】\033[m  厂商: %s\n", info->vendor);
    printf("\033[1;94m【设备类型】\033[m  型号: %s\n", info->model);
    if (info->is_rotational == 1) {
        printf("\033[1;94m【设备类型】\033[m  机械硬盘: 是\n");
        if (info->rpm > 0) {
            printf("\033[1;94m【设备类型】\033[m  转速: %d RPM\n", info->rpm);
        } else {
            printf("\033[1;94m【设备类型】\033[m  转速: 未知\n");
        }
    } else if (info->is_rotational == 0) {
        printf("\033[1;94m【设备类型】\033[m  固态硬盘: 是\n");
    }

    return 0;
}

// 根据设备类型推荐可疑块阈值
int get_recommended_suspect_threshold(const DeviceTypeInfo *dev_info) {
    if (dev_info->is_rotational == 0) {
        // SSD/NVMe
        return 20;
    } else if (dev_info->is_rotational == 1) {
        // 机械硬盘
        if (dev_info->rpm >= 10000) {
            return 60;  // 高速硬盘
        } else if (dev_info->rpm >= 7200 || dev_info->rpm == 0) {
            return 100; // 7200 RPM
        } else {
            return 150; // 5400 RPM或更慢
        }
    }
    return DEFAULT_SUSPECT_THRESHOLD; // 未知类型
}

// 根据设备类型生成默认配置
int generate_auto_config(const DeviceTypeInfo *dev_type_info, TimeCategory *cats) {
    int count = 0;

    if (dev_type_info->is_rotational == 0) {
        // SSD/NVMe 配置
        strcpy(cats[0].name, "极佳"); cats[0].max_time = 1;    strcpy(cats[0].color, "\033[1;32m");
        strcpy(cats[1].name, "优秀"); cats[1].max_time = 3;    strcpy(cats[1].color, "\033[32m");
        strcpy(cats[2].name, "良好"); cats[2].max_time = 5;    strcpy(cats[2].color, "\033[36m");
        strcpy(cats[3].name, "正常"); cats[3].max_time = 10;   strcpy(cats[3].color, "\033[33m");
        strcpy(cats[4].name, "偏慢"); cats[4].max_time = 20;   strcpy(cats[4].color, "\033[35m");
        strcpy(cats[5].name, "异常"); cats[5].max_time = 50;   strcpy(cats[5].color, "\033[31m");
        strcpy(cats[6].name, "严重"); cats[6].max_time = 100;  strcpy(cats[6].color, "\033[1;31m");
        count = 7;
    } else if (dev_type_info->is_rotational == 1) {
        // 机械硬盘配置
        if (dev_type_info->rpm >= 10000) {
            // 高速硬盘 (10K/15K RPM)
            strcpy(cats[0].name, "优秀"); cats[0].max_time = 6;    strcpy(cats[0].color, "\033[1;32m");
            strcpy(cats[1].name, "良好"); cats[1].max_time = 12;   strcpy(cats[1].color, "\033[32m");
            strcpy(cats[2].name, "正常"); cats[2].max_time = 20;   strcpy(cats[2].color, "\033[36m");
            strcpy(cats[3].name, "偏慢"); cats[3].max_time = 40;   strcpy(cats[3].color, "\033[33m");
            strcpy(cats[4].name, "较慢"); cats[4].max_time = 80;   strcpy(cats[4].color, "\033[35m");
            strcpy(cats[5].name, "很慢"); cats[5].max_time = 150;  strcpy(cats[5].color, "\033[31m");
            strcpy(cats[6].name, "极慢"); cats[6].max_time = 300;  strcpy(cats[6].color, "\033[1;31m");
        } else if (dev_type_info->rpm >= 7200 || dev_type_info->rpm == 0) {
            // 7200 RPM 或未知转速
            strcpy(cats[0].name, "优秀"); cats[0].max_time = 8;    strcpy(cats[0].color, "\033[1;32m");
            strcpy(cats[1].name, "良好"); cats[1].max_time = 15;   strcpy(cats[1].color, "\033[32m");
            strcpy(cats[2].name, "正常"); cats[2].max_time = 25;   strcpy(cats[2].color, "\033[36m");
            strcpy(cats[3].name, "偏慢"); cats[3].max_time = 50;   strcpy(cats[3].color, "\033[33m");
            strcpy(cats[4].name, "较慢"); cats[4].max_time = 100;  strcpy(cats[4].color, "\033[35m");
            strcpy(cats[5].name, "很慢"); cats[5].max_time = 200;  strcpy(cats[5].color, "\033[31m");
            strcpy(cats[6].name, "极慢"); cats[6].max_time = 500;  strcpy(cats[6].color, "\033[1;31m");
        } else {
            // 5400 RPM 或更慢
            strcpy(cats[0].name, "优秀"); cats[0].max_time = 12;   strcpy(cats[0].color, "\033[1;32m");
            strcpy(cats[1].name, "良好"); cats[1].max_time = 25;   strcpy(cats[1].color, "\033[32m");
            strcpy(cats[2].name, "正常"); cats[2].max_time = 40;   strcpy(cats[2].color, "\033[36m");
            strcpy(cats[3].name, "偏慢"); cats[3].max_time = 80;   strcpy(cats[3].color, "\033[33m");
            strcpy(cats[4].name, "较慢"); cats[4].max_time = 150;  strcpy(cats[4].color, "\033[35m");
            strcpy(cats[5].name, "很慢"); cats[5].max_time = 300;  strcpy(cats[5].color, "\033[31m");
            strcpy(cats[6].name, "极慢"); cats[6].max_time = 600;  strcpy(cats[6].color, "\033[1;31m");
        }
        count = 7;
    } else {
        // 未知设备类型，使用保守配置
        strcpy(cats[0].name, "优秀"); cats[0].max_time = 50;    strcpy(cats[0].color, "\033[1;32m");
        strcpy(cats[1].name, "良好"); cats[1].max_time = 100;   strcpy(cats[1].color, "\033[32m");
        strcpy(cats[2].name, "一般"); cats[2].max_time = 200;   strcpy(cats[2].color, "\033[36m");
        strcpy(cats[3].name, "较差"); cats[3].max_time = 500;   strcpy(cats[3].color, "\033[33m");
        strcpy(cats[4].name, "很差"); cats[4].max_time = 1000;  strcpy(cats[4].color, "\033[35m");
        strcpy(cats[5].name, "严重"); cats[5].max_time = 3000;  strcpy(cats[5].color, "\033[31m");
        count = 6;
    }

    int recommended = get_recommended_suspect_threshold(dev_type_info);
    strcpy(cats[count].name, "可疑"); cats[count].max_time = recommended;
    strcpy(cats[count].color, "\033[1;33m");
    count++;
    strcpy(cats[count].name, "坏道"); cats[count].max_time = cats[count - 2].max_time;
    strcpy(cats[count].color, "\033[1;31m");
    count++;

    // 初始化计数器
    for (int i = 0; i < count; i++) {
        cats[i].count = 0;
    }

    return count;
}

typedef struct {
    int             sector_size;
    unsigned long   total_sectors;
    unsigned long   start_sector;
    unsigned long   end_sector;
    unsigned long   sector_count;
    unsigned long   block_count;
    unsigned long   sector_offset;
    int             sectors_per_block;
} DeviceInfo;

// 获取设备信息并解析范围参数
int get_device_info(const char *device, const char *start_str, const char *end_str,
                   size_t block_size, DeviceInfo *info) {
    // 获取设备信息
    int fd_info = open(device, O_RDONLY);
    if (fd_info == -1) {
        perror("打开设备失败");
        return -1;
    }

    // 获取物理扇区大小
    info->sector_size = 512;
    if (ioctl(fd_info, BLKSSZGET, &info->sector_size) == -1) {
        fprintf(stderr, "警告: 无法获取扇区大小，使用默认值 %d\n", info->sector_size);
    }

    printf("\033[35m【设备信息】\033[m物理扇区大小: %d 字节\n", info->sector_size);

    // 获取总扇区数
    info->total_sectors = 0;
    if (ioctl(fd_info, BLKGETSIZE, &info->total_sectors) == -1) {
        fprintf(stderr, "警告: 无法获取总扇区数\n");
    } else {
        printf("\033[35m【设备信息】\033[m设备总扇区数: %lu (%.2f GB)\n", info->total_sectors,
               (double)info->total_sectors * 512 / (1024*1024*1024));
    }
    close(fd_info);

    // 解析起始和结束参数
    if (strchr(start_str, '%')) {
        info->start_sector = parse_percentage(start_str, info->total_sectors);
    } else {
        info->start_sector = strtoul(start_str, NULL, 0);
    }

    if (strchr(end_str, '%')) {
        info->end_sector = parse_percentage(end_str, info->total_sectors);
    } else {
        info->end_sector = strtoul(end_str, NULL, 0);
    }

    // 确保结束扇区不超过设备限制
    if (info->total_sectors > 0 && info->end_sector > info->total_sectors - 1) {
        info->end_sector = info->total_sectors - 1;
    }

    // 验证块大小
    if (block_size % info->sector_size != 0) {
        fprintf(stderr, "错误: 块大小(%zu)必须是扇区大小(%d)的倍数\n",
                block_size, info->sector_size);
        return -1;
    }

    // 计算每块包含的扇区数
    info->sectors_per_block = block_size / info->sector_size;

    // 调整起始位置使其按块对齐
    unsigned long aligned_start_sector = (info->start_sector / info->sectors_per_block) * info->sectors_per_block;
    if (aligned_start_sector < info->start_sector) {
        printf("\033[35m【设备信息】\033[m调整起始扇区: %lu -> %lu (按块大小对齐)\n",
               info->start_sector, aligned_start_sector);
        info->start_sector = aligned_start_sector;
    }

    // 调整结束位置使其按块对齐
    unsigned long aligned_end_sector = ((info->end_sector + info->sectors_per_block) / info->sectors_per_block) * info->sectors_per_block - 1;
    if (aligned_end_sector > info->end_sector) {
        printf("\033[35m【设备信息】\033[m调整结束扇区: %lu -> %lu (按块大小对齐)\n",
               info->end_sector, aligned_end_sector);
        info->end_sector = aligned_end_sector;
    }

    // 计算实际测试的块数
    info->sector_count = info->end_sector - info->start_sector + 1;
    info->block_count = info->sector_count / info->sectors_per_block;
    info->sector_offset = info->start_sector;

    if (info->sector_count % info->sectors_per_block != 0) {
        fprintf(stderr, "错误: 扇区范围 %lu-%lu (%lu 扇区) 不是块大小(%d 扇区)的倍数\n",
                info->start_sector, info->end_sector, info->sector_count, info->sectors_per_block);
        return -1;
    }

    printf("\033[35m【设备信息】\033[m已决定测试范围为: 扇区 %lu - %lu (共 %lu 扇区, %lu 块)\n",
           info->start_sector, info->end_sector, info->sector_count, info->block_count);

    return 0;
}

// 初始化扫描环境
int initialize_scan(const char *device, size_t block_size, const DeviceInfo *info,
                   int *fd, void **buffer, FILE **logfile, const char *log_filename) {
    // 以 O_DIRECT 方式打开设备
    *fd = open(device, O_RDONLY | O_DIRECT | O_SYNC);
    if (*fd == -1) {
        perror("打开设备失败");
        return -1;
    }

    // 分配对齐的内存缓冲区
    long page_size = sysconf(_SC_PAGESIZE);
    size_t align_size = (info->sector_size > page_size) ? info->sector_size : page_size;

    if (posix_memalign(buffer, align_size, block_size)) {
        perror("内存分配失败");
        close(*fd);
        return -1;
    }

    // 打开日志文件
    *logfile = NULL;
    if (log_filename) {
        *logfile = fopen(log_filename, "a");
        if (*logfile == NULL) {
            fprintf(stderr, "警告: 无法创建日志文件 '%s': %s\n",
                    log_filename, strerror(errno));
        } else {
            fprintf(*logfile, "# Disk Health Scan Report\n");
            fprintf(*logfile, "# Device: %s\n", device);
            fprintf(*logfile, "# Start sector: %lu\n", info->start_sector);
            fprintf(*logfile, "# End sector: %lu\n", info->end_sector);
            fprintf(*logfile, "# Sector size: %d bytes\n", info->sector_size);
            fprintf(*logfile, "# Block size: %zu bytes\n", block_size);
            fprintf(*logfile, "# Timestamp: %ld\n", (long)time(NULL));
            fprintf(*logfile, "# Format: <sector> # <timestamp> # <latency> # <status>\n");
            fprintf(*logfile, "# ================================================\n");
            fflush(*logfile);
            printf("日志文件: %s (记录非优秀/良好/一般的区块)\n", log_filename);
        }
    }

    return 0;
}

// 执行主要的扫描过程
void perform_scan(int fd, void *buffer, const ScanOptions *opts, const DeviceInfo *info,
                 TimeCategory *categories, int cat_count, FILE *logfile) {

    // 初始化采样迭代器
    SampleIterator iterator;
    init_sample_iterator(&iterator, info->block_count, opts->sample_ratio, opts->random_sampling);

    printf("\033[1;37m【采样策略】\033[m计划扫描块数: %lu (共 %lu 块)\n", iterator.total_samples, info->block_count);
    // 判断是否为顺序扫描 (100% 均匀采样)
    int is_sequential = (opts->sample_ratio >= 100.0 && !opts->random_sampling);
    printf("\033[1;37m【采样策略】\033[m扫描策略: \033[32m%s\033[m\n", is_sequential ? "顺序全量扫描" : "跳跃式前进扫描");
    if (!is_sequential) {
        printf("\033[1;37m【采样策略】\033[m抽样比例: %.1f%%\n", opts->sample_ratio);
        printf("\033[1;37m【采样策略】\033[m采样模式: \033[32m%s\033[m\n", opts->random_sampling ? "随机采样" : "均匀采样");
    }
    if (opts->wait_factor > 0) {
        printf("\033[1;37m【采样策略】\033[m等待时间因子: %d%%\n", opts->wait_factor);
    }
    printf("\033[1;37m【采样策略】\033[m可疑块阈值: %d ms (重测 %d 次，间隔 %d ms)\n",
           opts->suspect_threshold, opts->suspect_retries, opts->suspect_interval);

    // 计算报告间隔
    unsigned long report_interval = iterator.total_samples / 100;
    if (report_interval < MIN_REPORT_INTERVAL) report_interval = MIN_REPORT_INTERVAL;
    if (report_interval > 100000) report_interval = 100000;

    struct timespec global_start;
    clock_gettime(CLOCK_MONOTONIC, &global_start);

    printf("========================================\n");

    print_progress_report(0, iterator.total_samples, categories, cat_count, &global_start);

    struct timespec block_start, block_end;
    const char *default_status = "未知";
    long last_elapsed = 0;  // 用于计算等待时间
    unsigned long processed = 0;
    long prev_block = -1;

    long current_block;
    while ((current_block = get_next_sample_block(&iterator)) != -1) {
        unsigned long block = (unsigned long)current_block;
        long elapsed = 0;
        const char *status = default_status;
        int status_index = cat_count - 1; // 默认为最后一个分类（坏道）
        int is_suspect = 0;

        processed++;

        // 等待时间处理
        if (opts->wait_factor > 0 && last_elapsed > 0) {
            long wait_time_ms = (last_elapsed * opts->wait_factor) / 100;
            if (wait_time_ms > 0) {
                struct timespec wait_time = {
                    .tv_sec = wait_time_ms / 1000,
                    .tv_nsec = (wait_time_ms % 1000) * 1000000
                };
                nanosleep(&wait_time, NULL);
            }
        }

        // 优化：顺序扫描且连续块时不需要lseek
        if (!is_sequential || prev_block == -1 || block != (unsigned long)(prev_block + 1)) {
            // 需要定位
            off_t block_offset = (off_t)(block * info->sectors_per_block + info->sector_offset) * info->sector_size;
            if (lseek(fd, block_offset, SEEK_SET) != block_offset) {
                status = "定位错误";
                elapsed = 1000000;
                goto record_result;
            }
        }

        prev_block = (long)block;

        clock_gettime(CLOCK_MONOTONIC, &block_start);
        ssize_t bytes_read = read(fd, buffer, opts->block_size);
        clock_gettime(CLOCK_MONOTONIC, &block_end);

        if (bytes_read != (ssize_t)opts->block_size) {
            status = "读取错误";
            elapsed = 1000000;
        } else {
            elapsed = (block_end.tv_sec - block_start.tv_sec) * 1000 +
                      (block_end.tv_nsec - block_start.tv_nsec) / 1000000;

            // 检查是否为可疑块
            if (elapsed > opts->suspect_threshold) {
                is_suspect = 1;
                categories[cat_count - 2].count++; // 可疑分类计数

                // 进行重测
                long retest_result = retest_suspect_block(fd, buffer, opts->block_size,
                                                        block, info->sectors_per_block,
                                                        info->sector_offset, info->sector_size,
                                                        opts->suspect_retries,
                                                        opts->suspect_interval);

                if (retest_result < 0) {
                    status = "读取错误";
                    elapsed = 1000000;
                }
                else {
                    elapsed = retest_result;
                }
            }

            // 查找匹配的分类（排除可疑分类）
            for (int j = 0; j < cat_count - 2; j++) { // 排除可疑和坏道分类
                if (elapsed <= categories[j].max_time) {
                    if (!is_suspect) categories[j].count++; // 只有非可疑块才增加计数
                    status = categories[j].name;
                    status_index = j;
                    break;
                }
            }

            // 如果没有匹配到任何分类，归为坏道
            if (status_index == cat_count - 1 && !is_suspect) {
                categories[cat_count - 1].count++; // 坏道分类
                status = categories[cat_count - 1].name;
            }
        }

record_result:
        last_elapsed = elapsed;

        // 记录速度不好的区块
        if (logfile && elapsed > opts->log_threshold) {
            log_block(logfile, block, info->sector_offset, opts->block_size,
                      info->sectors_per_block, elapsed, status);
        }

        // 定期显示进度
        if (processed == 1 || processed == iterator.total_samples || processed % report_interval == 0) {
            print_progress_report(processed, iterator.total_samples, categories, cat_count, &global_start);
        }
    }

    print_progress_report(iterator.total_samples, iterator.total_samples, categories, cat_count, &global_start);
    printf("\n\n");
}

// 生成最终报告
void generate_final_report(const ScanOptions *opts, const DeviceInfo *info,
                          TimeCategory *categories, int cat_count,
                          struct timespec *start_time, FILE *logfile) {
    // 计算总时间
    struct timespec global_end;
    clock_gettime(CLOCK_MONOTONIC, &global_end);
    double total_sec = (global_end.tv_sec - start_time->tv_sec) +
                      (global_end.tv_nsec - start_time->tv_nsec) / 1000000000.0;

    // 计算实际测试的块数（不包括可疑分类）
    unsigned long actual_tested_blocks = 0;
    unsigned long sample_count = (unsigned long)(info->block_count * opts->sample_ratio / 100.0);
    if (sample_count == 0) sample_count = 1;
    if (sample_count > info->block_count) sample_count = info->block_count;

    for (int i = 0; i < cat_count; i++) {
        if (i != cat_count - 2) { // 排除可疑分类
            actual_tested_blocks += categories[i].count;
        }
    }

    // 打印最终报告
    printf("\n===== 坏道检测报告 =====\n");
    printf("总扇区数: %lu (%.2f GB)\n", info->sector_count,
           (double)info->sector_count * info->sector_size / (1024*1024*1024));
    printf("总块数: %lu\n", info->block_count);
    printf("抽样扫描块数: %lu (%.2f%%)\n", sample_count,
           100.0 * sample_count / info->block_count);
    printf("实际测试块数: %lu\n", actual_tested_blocks);
    printf("总扫描时间: %.1f 秒\n", total_sec);

    if (total_sec > 0) {
        double mb_speed = (sample_count * opts->block_size) / (total_sec * 1024 * 1024);
        printf("平均速度: %.1f MB/s\n", mb_speed);
    }

    printf("------------------------\n");

    for (int i = 0; i < cat_count; i++) {
        if (i == cat_count - 2) {
            // 可疑分类单独处理
            printf("%s%-6s\033[0m (>%4d ms): %8lu 块 (重测后重新分类)\n",
                   categories[i].color, categories[i].name, opts->suspect_threshold,
                   categories[i].count);
        } else if (i < cat_count - 2) {
            printf("%s%-6s\033[0m (≤%4ld ms): %8lu 块 (%5.2f%%)\n",
                   categories[i].color, categories[i].name, categories[i].max_time,
                   categories[i].count,
                   actual_tested_blocks > 0 ? 100.0 * categories[i].count / actual_tested_blocks : 0.0);
        } else {
            // 坏道分类
            printf("%s%-6s\033[0m (>%4ld ms): %8lu 块 (%5.2f%%)\n",
                   categories[i].color, categories[i].name,
                   cat_count > 2 ? categories[cat_count-3].max_time : 3000,
                   categories[i].count,
                   actual_tested_blocks > 0 ? 100.0 * categories[i].count / actual_tested_blocks : 0.0);
        }
    }

    // 写入日志统计
    if (logfile) {
        fprintf(logfile, "# ========== 扫描统计 ==========\n");
        fprintf(logfile, "# 总扇区数: %lu\n", info->sector_count);
        fprintf(logfile, "# 总块数: %lu\n", info->block_count);
        fprintf(logfile, "# 抽样扫描块数: %lu (%.2f%%)\n", sample_count,
                100.0 * sample_count / info->block_count);
        fprintf(logfile, "# 实际测试块数: %lu\n", actual_tested_blocks);
        fprintf(logfile, "# 总扫描时间: %.1f 秒\n", total_sec);

        if (total_sec > 0) {
            double mb_speed = (sample_count * opts->block_size) / (total_sec * 1024 * 1024);
            fprintf(logfile, "# 平均速度: %.1f MB/s\n", mb_speed);
        }

        fprintf(logfile, "# ------------------------\n");
        for (int i = 0; i < cat_count; i++) {
            if (i == cat_count - 2) {
                fprintf(logfile, "# %-6s (> %4d ms): %8lu (重测后重新分类)\n",
                       categories[i].name, opts->suspect_threshold, categories[i].count);
            } else if (i < cat_count - 2) {
                fprintf(logfile, "# %-6s (≤ %4ld ms): %8lu (%5.2f%%)\n",
                       categories[i].name, categories[i].max_time,
                       categories[i].count,
                       actual_tested_blocks > 0 ? 100.0 * categories[i].count / actual_tested_blocks : 0.0);
            } else {
                fprintf(logfile, "# %-6s (> %4ld ms): %8lu (%5.2f%%)\n",
                       categories[i].name,
                       cat_count > 2 ? categories[cat_count-3].max_time : 3000,
                       categories[i].count,
                       actual_tested_blocks > 0 ? 100.0 * categories[i].count / actual_tested_blocks : 0.0);
            }
        }
        fprintf(logfile, "# 扫描完成时间: %ld\n", (long)time(NULL));
    }
}

int main(int argc, char *argv[]) {
    ScanOptions opts;
    DeviceInfo device_info;
    DeviceTypeInfo device_type_info;
    TimeCategory categories[MAX_CATEGORIES];
    int cat_count = 0;
    int fd = -1;
    void *buffer = NULL;
    FILE *logfile = NULL;

    // 解析命令行参数
    if (parse_arguments(argc, argv, &opts) != 0) {
        return 1;
    }

    // 获取设备信息
    if (get_device_info(opts.device, opts.start_str, opts.end_str, opts.block_size, &device_info) != 0) {
        return 1;
    }

    // 检测设备类型
    if (detect_device_type(opts.device, &device_type_info) == 0) {
        // 如果用户没有指定可疑块阈值，使用推荐值
        if (opts.suspect_threshold == DEFAULT_SUSPECT_THRESHOLD) {
            int recommended = get_recommended_suspect_threshold(&device_type_info);
            if (recommended != DEFAULT_SUSPECT_THRESHOLD) {
                opts.suspect_threshold = recommended;
                printf("\033[33m【准备扫描】\033[m根据设备类型自动调整可疑块阈值为: %d ms\n", recommended);
            }
        }
    } else {
        printf("\033[33m【准备扫描】\033[m警告: 无法检测设备类型，将使用默认配置\n");
    }

    // 加载时间分类配置
    if (opts.config_file) {
        cat_count = load_categories(opts.config_file, categories);
    }

    if (cat_count <= 0) {
        // 尝试根据设备类型生成自动配置
        cat_count = generate_auto_config(&device_type_info, categories);
        printf("\033[33m【准备扫描】\033[m已根据设备类型自动生成了时间分类配置。\n");
        // 设置可疑块阈值
        categories[cat_count - 2].max_time = opts.suspect_threshold;
        // 坏道分类使用最后一个正常分类的阈值（但是方向相反，上限变下限）
        categories[cat_count - 1].max_time = categories[cat_count - 3].max_time;
    }

    // 初始化扫描环境
    if (initialize_scan(opts.device, opts.block_size, &device_info, &fd, &buffer, &logfile, opts.log_filename) != 0) {
        return 1;
    }

    // 初始化分类计数器
    for (int i = 0; i < cat_count; i++) {
        categories[i].count = 0;
    }

    // 打印扫描信息
    // printf("扇区偏移量: %lu\n", device_info.sector_offset);
    print_category_definitions(categories, cat_count);

    // 设置初始位置
    off_t start_offset = (off_t)device_info.start_sector * device_info.sector_size;
    if (lseek(fd, start_offset, SEEK_SET) != start_offset) {
        perror("设置初始位置失败");
        goto cleanup;
    }

    struct timespec scan_start;
    clock_gettime(CLOCK_MONOTONIC, &scan_start);

    // 执行扫描
    perform_scan(fd, buffer, &opts, &device_info, categories, cat_count, logfile);

    // 生成最终报告
    generate_final_report(&opts, &device_info, categories, cat_count, &scan_start, logfile);

cleanup:
    if (buffer) free(buffer);
    if (fd >= 0) close(fd);
    if (logfile) fclose(logfile);

    return 0;
}
