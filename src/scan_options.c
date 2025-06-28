// src/scan_options.c - 命令行参数解析和选项处理

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>

#include "scan_options.h"

// 内部函数声明
static void print_usage(const char *program_name);
static unsigned long parse_percentage(const char *str, unsigned long total_sectors);
static int validate_options(const ScanOptions *opts);
static void print_scan_summary(const ScanOptions *opts);

/**
 * 打印程序使用帮助
 *
 * @param program_name 程序名称
 */
static void print_usage(const char *program_name) {
    printf("\033[1;32m磁盘健康扫描工具 v2.0\033[0m\n");
    printf("用于检测磁盘坏块和性能问题的专业工具\n\n");

    printf("\033[1;36m用法:\033[0m\n");
    printf("  %s [选项] 设备路径 起始位置 结束位置\n\n", program_name);

    printf("\033[1;36m基本选项:\033[0m\n");
    printf("  -b, --block-size SIZE    读取块大小 (默认: %d 字节)\n", BLOCK_SIZE_DEFAULT);
    printf("                           建议值: 4096, 8192, 16384, 65536\n");
    printf("  -l, --log FILE           日志文件路径 (记录详细扫描结果)\n");
    printf("  -t, --log-threshold MS   记录到日志的延迟阈值 (毫秒, 默认: 所有延迟)\n");
    printf("  -c, --config FILE        时间分类配置文件\n");
    printf("  -h, --help               显示此帮助信息\n\n");

    printf("\033[1;36m采样选项:\033[0m\n");
    printf("  -s, --sample RATIO       采样比例 (0.0-1.0, 1.0=全量扫描, 默认: 1.0)\n");
    printf("  -r, --random             使用随机采样而非等间距采样\n");
    printf("  -w, --wait FACTOR        等待因子 (延迟倍数, 用于降低磁盘负载)\n\n");

    printf("\033[1;36m可疑块处理:\033[0m\n");
    printf("  -S, --suspect MS         可疑块阈值 (毫秒, 0=自动根据设备类型)\n");
    printf("  -R, --retries NUM        可疑块重测次数 (默认: %d)\n", DEFAULT_SUSPECT_RETRIES);
    printf("  -I, --interval MS        重测间隔 (毫秒, 默认: %d)\n", DEFAULT_SUSPECT_INTERVAL);
    printf("                           重测可以避免瞬时干扰导致的误判\n\n");

    printf("\033[1;36m位置参数:\033[0m\n");
    printf("  设备路径                 要扫描的块设备 (如 /dev/sda, /dev/nvme0n1)\n");
    printf("  起始位置                 起始扇区号或百分比 (如 0, 1000, 10%%)\n");
    printf("  结束位置                 结束扇区号或百分比 (如 1000000, 50%%, 100%%)\n\n");

    printf("\033[1;36m使用示例:\033[0m\n");
    printf("  \033[1;33m基本全盘扫描:\033[0m\n");
    printf("    %s /dev/sda 0 100%%\n\n", program_name);

    printf("  \033[1;33m扫描特定扇区范围:\033[0m\n");
    printf("    %s /dev/sda 0 1000000\n\n", program_name);

    printf("  \033[1;33m采样扫描（1%% 采样率）:\033[0m\n");
    printf("    %s /dev/sda 0%% 100%% -s 0.01\n\n", program_name);

    printf("  \033[1;33m高性能 NVMe 扫描:\033[0m\n");
    printf("    %s -b 4096 -S 10 /dev/nvme0n1 0 100%%\n\n", program_name);

    printf("  \033[1;33m详细日志记录:\033[0m\n");
    printf("    %s -l scan.log -t 50 /dev/sda 0%% 10%%\n\n", program_name);

    printf("  \033[1;33m低负载扫描:\033[0m\n");
    printf("    %s -w 2 -s 0.1 /dev/sda 0 100%%\n\n", program_name);

    printf("\033[1;36m注意事项:\033[0m\n");
    printf("  • 扫描期间会产生大量磁盘 I/O，建议在系统空闲时进行\n");
    printf("  • 对于 SSD，建议使用较大的块大小（4KB 或以上）\n");
    printf("  • 可疑块阈值应根据设备类型调整：NVMe SSD < SATA SSD < HDD\n");
    printf("  • 使用采样扫描可以快速发现明显问题，节省时间\n");
    printf("  • 建议先进行采样扫描，发现问题区域后再进行全量扫描\n\n");
}

/**
 * 解析百分比参数
 *
 * @param str 百分比字符串（如 "10%"）
 * @param total_sectors 总扇区数
 * @return 对应的扇区号
 */
static unsigned long parse_percentage(const char *str, unsigned long total_sectors) {
    if (!str) {
        fprintf(stderr, "错误: 空的百分比参数\n");
        exit(1);
    }

    char *endptr;
    double percent = strtod(str, &endptr);

    if (*endptr == '%' && percent >= 0 && percent <= 100) {
        unsigned long result = (unsigned long)(percent / 100.0 * total_sectors);
        printf("\033[36m【参数解析】\033[m百分比 %s 对应扇区: %lu (总扇区: %lu)\n",
               str, result, total_sectors);
        return result;
    }

    fprintf(stderr, "错误: 无效的百分比参数 '%s'，应该是 0%%-100%% 格式\n", str);
    exit(1);
}

/**
 * 验证选项的合理性
 *
 * @param opts 扫描选项结构
 * @return 0 成功，-1 失败
 */
static int validate_options(const ScanOptions *opts) {
    // 验证块大小
    if (opts->block_size < 512) {
        fprintf(stderr, "错误: 块大小不能小于 512 字节\n");
        return -1;
    }

    if (opts->block_size > 1024*1024) {
        fprintf(stderr, "错误: 块大小不能大于 1MB\n");
        return -1;
    }

    // 检查块大小是否是 512 的倍数
    if (opts->block_size % 512 != 0) {
        fprintf(stderr, "错误: 块大小必须是 512 字节的倍数\n");
        return -1;
    }

    // 验证采样比例
    if (opts->sample_ratio <= 0 || opts->sample_ratio > 1.0) {
        fprintf(stderr, "错误: 采样比例必须在 0.0-1.0 之间\n");
        return -1;
    }

    // 验证等待因子
    if (opts->wait_factor < 0) {
        fprintf(stderr, "错误: 等待因子不能为负数\n");
        return -1;
    }

    // 验证可疑块参数
    if (opts->suspect_threshold < 0) {
        fprintf(stderr, "错误: 可疑块阈值不能为负数\n");
        return -1;
    }

    if (opts->suspect_retries < 0 || opts->suspect_retries > 100) {
        fprintf(stderr, "错误: 重测次数必须在 0-100 之间\n");
        return -1;
    }

    if (opts->suspect_interval < 0) {
        fprintf(stderr, "错误: 重测间隔不能为负数\n");
        return -1;
    }

    // 验证日志阈值
    if (opts->log_threshold < 0) {
        fprintf(stderr, "错误: 日志阈值不能为负数\n");
        return -1;
    }

    // 验证设备路径
    if (access(opts->device, R_OK) != 0) {
        fprintf(stderr, "错误: 无法访问设备 '%s': %s\n", opts->device, strerror(errno));
        return -1;
    }

    return 0;
}

/**
 * 打印扫描参数摘要
 *
 * @param opts 扫描选项结构
 */
static void print_scan_summary(const ScanOptions *opts) {
    printf("\033[1;36m【扫描参数摘要】\033[0m\n");
    printf("设备路径: %s\n", opts->device);
    printf("起始位置: %s\n", opts->start_str);
    printf("结束位置: %s\n", opts->end_str);
    printf("块大小: %zu 字节\n", opts->block_size);

    if (opts->sample_ratio < 1.0) {
        printf("采样比例: %.2f%% (%s)\n",
               opts->sample_ratio * 100,
               opts->random_sampling ? "随机采样" : "等间距采样");
    } else {
        printf("扫描模式: 全量扫描\n");
    }

    if (opts->wait_factor > 0) {
        printf("等待因子: %d (降低磁盘负载)\n", opts->wait_factor);
    }

    printf("可疑块阈值: %d 毫秒%s\n",
           opts->suspect_threshold,
           opts->suspect_threshold == DEFAULT_SUSPECT_THRESHOLD ? " (自动)" : "");

    if (opts->suspect_retries > 0) {
        printf("可疑块重测: %d 次，间隔 %d 毫秒\n",
               opts->suspect_retries, opts->suspect_interval);
    }

    if (opts->log_filename) {
        printf("日志文件: %s", opts->log_filename);
        if (opts->log_threshold > 0) {
            printf(" (阈值: %d 毫秒)", opts->log_threshold);
        }
        printf("\n");
    }

    if (opts->config_file) {
        printf("配置文件: %s\n", opts->config_file);
    }

    printf("\n");
}

/**
 * 解析命令行参数
 *
 * @param argc 参数个数
 * @param argv 参数数组
 * @param opts 输出的选项结构
 * @return 0 成功，-1 失败
 */
int parse_arguments(int argc, char *argv[], ScanOptions *opts) {
    if (!opts) {
        fprintf(stderr, "错误: 选项结构指针为空\n");
        return -1;
    }

    // 设置默认值
    opts->block_size = BLOCK_SIZE_DEFAULT;
    opts->log_filename = NULL;
    opts->log_threshold = 0;  // 0 表示记录所有延迟
    opts->config_file = NULL;
    opts->sample_ratio = 1.0;  // 默认全量扫描
    opts->random_sampling = 0;
    opts->wait_factor = 0;
    opts->suspect_threshold = DEFAULT_SUSPECT_THRESHOLD;  // 将由设备类型自动调整
    opts->suspect_retries = DEFAULT_SUSPECT_RETRIES;
    opts->suspect_interval = DEFAULT_SUSPECT_INTERVAL;
    opts->device = NULL;
    opts->start_str = NULL;
    opts->end_str = NULL;

    static struct option long_options[] = {
        {"block-size",    required_argument, 0, 'b'},
        {"log",           required_argument, 0, 'l'},
        {"log-threshold", required_argument, 0, 't'},
        {"config",        required_argument, 0, 'c'},
        {"sample",        required_argument, 0, 's'},
        {"random",        no_argument,       0, 'r'},
        {"wait",          required_argument, 0, 'w'},
        {"suspect",       required_argument, 0, 'S'},
        {"retries",       required_argument, 0, 'R'},
        {"interval",      required_argument, 0, 'I'},
        {"help",          no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    int option_index = 0;

    while ((c = getopt_long(argc, argv, "b:l:t:c:s:rw:S:R:I:h", long_options, &option_index)) != -1) {
        switch (c) {
            case 'b':
                opts->block_size = atoi(optarg);
                if (opts->block_size <= 0) {
                    fprintf(stderr, "错误: 无效的块大小 '%s'\n", optarg);
                    return -1;
                }
                break;

            case 'l':
                opts->log_filename = optarg;
                break;

            case 't':
                opts->log_threshold = atoi(optarg);
                break;

            case 'c':
                opts->config_file = optarg;
                // 检查配置文件是否存在
                if (access(opts->config_file, R_OK) != 0) {
                    fprintf(stderr, "警告: 无法访问配置文件 '%s': %s\n",
                            opts->config_file, strerror(errno));
                }
                break;

            case 's':
                opts->sample_ratio = atof(optarg);
                break;

            case 'r':
                opts->random_sampling = 1;
                break;

            case 'w':
                opts->wait_factor = atoi(optarg);
                break;

            case 'S':
                opts->suspect_threshold = atoi(optarg);
                break;

            case 'R':
                opts->suspect_retries = atoi(optarg);
                break;

            case 'I':
                opts->suspect_interval = atoi(optarg);
                break;

            case 'h':
                print_usage(argv[0]);
                exit(0);
                break;

            case '?':
                if (optopt == 'b' || optopt == 'l' || optopt == 't' ||
                    optopt == 'c' || optopt == 's' || optopt == 'w' ||
                    optopt == 'S' || optopt == 'R' || optopt == 'I') {
                    fprintf(stderr, "错误: 选项 -%c 需要参数\n", optopt);
                } else if (isprint(optopt)) {
                    fprintf(stderr, "错误: 未知选项 -%c\n", optopt);
                } else {
                    fprintf(stderr, "错误: 未知选项字符 \\x%x\n", optopt);
                }
                fprintf(stderr, "使用 %s --help 查看帮助信息\n", argv[0]);
                return -1;

            default:
                fprintf(stderr, "错误: getopt 返回了意外的字符代码 0%o\n", c);
                return -1;
        }
    }

    // 检查位置参数
    if (optind + 3 != argc) {
        fprintf(stderr, "错误: 需要提供设备路径、起始位置和结束位置\n");
        fprintf(stderr, "使用 %s --help 查看帮助信息\n", argv[0]);
        return -1;
    }

    opts->device = argv[optind];
    opts->start_str = argv[optind + 1];
    opts->end_str = argv[optind + 2];

    // 验证选项
    if (validate_options(opts) != 0) {
        return -1;
    }

    // 打印参数摘要
    print_scan_summary(opts);

    return 0;
}

/**
 * 解析位置参数（起始和结束位置）
 *
 * @param opts 扫描选项结构
 * @param total_sectors 设备总扇区数
 * @param start_sector 输出起始扇区
 * @param end_sector 输出结束扇区
 * @return 0 成功，-1 失败
 */
int parse_positions(const ScanOptions *opts, unsigned long total_sectors,
                   unsigned long *start_sector, unsigned long *end_sector) {
    if (!opts || !start_sector || !end_sector) {
        fprintf(stderr, "错误: 参数指针为空\n");
        return -1;
    }

    if (total_sectors == 0) {
        fprintf(stderr, "错误: 设备总扇区数为 0\n");
        return -1;
    }

    // 解析起始位置
    if (strchr(opts->start_str, '%')) {
        *start_sector = parse_percentage(opts->start_str, total_sectors);
    } else {
        char *endptr;
        *start_sector = strtoul(opts->start_str, &endptr, 10);
        if (*endptr != '\0') {
            fprintf(stderr, "错误: 无效的起始扇区 '%s'\n", opts->start_str);
            return -1;
        }
    }

    // 解析结束位置
    if (strchr(opts->end_str, '%')) {
        *end_sector = parse_percentage(opts->end_str, total_sectors);
    } else {
        char *endptr;
        *end_sector = strtoul(opts->end_str, &endptr, 10);
        if (*endptr != '\0') {
            fprintf(stderr, "错误: 无效的结束扇区 '%s'\n", opts->end_str);
            return -1;
        }
    }

    // 验证范围
    if (*start_sector >= total_sectors) {
        fprintf(stderr, "错误: 起始扇区 %lu 超出设备范围 (0-%lu)\n",
                *start_sector, total_sectors - 1);
        return -1;
    }

    if (*end_sector > total_sectors) {
        fprintf(stderr, "错误: 结束扇区 %lu 超出设备范围 (0-%lu)\n",
                *end_sector, total_sectors - 1);
        return -1;
    }

    if (*start_sector >= *end_sector) {
        fprintf(stderr, "错误: 起始扇区 %lu 必须小于结束扇区 %lu\n",
                *start_sector, *end_sector);
        return -1;
    }

    printf("\033[36m【位置解析】\033[m扫描范围: 扇区 %lu - %lu (共 %lu 个扇区)\n",
           *start_sector, *end_sector, *end_sector - *start_sector);

    // 如果是采样扫描，计算实际要扫描的扇区数
    if (opts->sample_ratio < 1.0) {
        unsigned long total_range = *end_sector - *start_sector;
        unsigned long sample_count = (unsigned long)(total_range * opts->sample_ratio);
        printf("\033[36m【采样计算】\033[m采样扇区数: %lu (采样率: %.2f%%)\n",
               sample_count, opts->sample_ratio * 100);
    }

    return 0;
}
