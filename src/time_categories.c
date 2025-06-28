// src/time_categories.c - 时间分类和配置管理

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>

#include "time_categories.h"
#include "device_info/device_info.h"

// 内部函数声明
static void set_default_categories(TimeCategories* categories, DeviceType device_type);
static int parse_config_line(const char* line, TimeCategories* categories);
static void trim_whitespace_inplace(char* str);
static void print_categories_summary(const TimeCategories* categories);
static const char* get_category_name(TimeCategoryType type);
static const char* get_category_color(TimeCategoryType type);
static const char* get_simple_device_type_name(DeviceType device_type);

/**
 * 初始化时间分类结构
 *
 * @param categories 时间分类结构指针
 * @param device_type 设备类型，用于设置默认阈值
 */
void initialize_time_categories(TimeCategories* categories, DeviceType device_type) {
    if (!categories) return;

    // 清零结构
    memset(categories, 0, sizeof(TimeCategories));

    // 根据设备类型设置默认分类
    set_default_categories(categories, device_type);

    printf("\033[32m【时间分类】\033[m已为 %s 设置默认时间分类\n",
           get_simple_device_type_name(device_type));
}

/**
 * 根据设备类型设置默认时间分类阈值
 *
 * @param categories 时间分类结构指针
 * @param device_type 设备类型
 */
static void set_default_categories(TimeCategories* categories, DeviceType device_type) {
    switch (device_type) {
        case DEVICE_TYPE_NVME_SSD:
            // NVMe SSD - 高性能，低延迟
            categories->excellent_max = 1;      // ≤ 1ms 优秀
            categories->good_max = 3;           // ≤ 3ms 良好
            categories->normal_max = 8;         // ≤ 8ms 正常
            categories->general_max = 20;       // ≤ 20ms 一般
            categories->poor_max = 50;          // ≤ 50ms 欠佳
            categories->severe_max = 200;       // ≤ 200ms 严重
            categories->suspect_threshold = 8;  // >= 8ms 可疑，需要重测（默认=正常上限）
            break;

        case DEVICE_TYPE_SATA_SSD:
        case DEVICE_TYPE_UNKNOWN_SSD:
            // SATA SSD - 中等性能
            categories->excellent_max = 2;      // ≤ 2ms 优秀
            categories->good_max = 8;           // ≤ 8ms 良好
            categories->normal_max = 20;        // ≤ 20ms 正常
            categories->general_max = 50;       // ≤ 50ms 一般
            categories->poor_max = 150;         // ≤ 150ms 欠佳
            categories->severe_max = 500;       // ≤ 500ms 严重
            categories->suspect_threshold = 20; // >= 20ms 可疑，需要重测（默认=正常上限）
            break;

        case DEVICE_TYPE_HDD:
            // 机械硬盘 - 较低性能，高延迟
            categories->excellent_max = 8;      // ≤ 8ms 优秀
            categories->good_max = 20;          // ≤ 20ms 良好
            categories->normal_max = 40;        // ≤ 40ms 正常
            categories->general_max = 80;       // ≤ 80ms 一般
            categories->poor_max = 200;         // ≤ 200ms 欠佳
            categories->severe_max = 1000;      // ≤ 1000ms 严重
            categories->suspect_threshold = 40; // >= 40ms 可疑，需要重测（默认=正常上限）
            break;

        case DEVICE_TYPE_USB_STORAGE:
            // USB 存储设备 - 性能变化较大
            categories->excellent_max = 5;      // ≤ 5ms 优秀
            categories->good_max = 15;          // ≤ 15ms 良好
            categories->normal_max = 40;        // ≤ 40ms 正常
            categories->general_max = 100;      // ≤ 100ms 一般
            categories->poor_max = 300;         // ≤ 300ms 欠佳
            categories->severe_max = 1500;      // ≤ 1500ms 严重
            categories->suspect_threshold = 40; // >= 40ms 可疑，需要重测（默认=正常上限）
            break;

        case DEVICE_TYPE_UNKNOWN:
        default:
            // 未知设备 - 使用保守的分类
            categories->excellent_max = 5;      // ≤ 5ms 优秀
            categories->good_max = 15;          // ≤ 15ms 良好
            categories->normal_max = 35;        // ≤ 35ms 正常
            categories->general_max = 80;       // ≤ 80ms 一般
            categories->poor_max = 200;         // ≤ 200ms 欠佳
            categories->severe_max = 800;       // ≤ 800ms 严重
            categories->suspect_threshold = 35; // >= 35ms 可疑，需要重测（默认=正常上限）
            break;
    }

    // 初始化统计计数器
    for (int i = 0; i < TIME_CATEGORY_COUNT; i++) {
        categories->counts[i] = 0;
    }

    categories->total_reads = 0;
    categories->total_time_ms = 0;
    categories->min_time_ms = 0;
    categories->max_time_ms = 0;
}

/**
 * 从配置文件加载时间分类设置
 *
 * @param categories 时间分类结构指针
 * @param config_file 配置文件路径
 * @return 0 成功，-1 失败
 */
int load_time_categories_config(TimeCategories* categories, const char* config_file) {
    if (!categories || !config_file) {
        fprintf(stderr, "错误: 参数为空\n");
        return -1;
    }

    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        fprintf(stderr, "错误: 无法打开配置文件 '%s': %s\n",
                config_file, strerror(errno));
        return -1;
    }

    printf("\033[32m【配置加载】\033[m正在加载配置文件: %s\n", config_file);

    char line[256];
    int line_number = 0;
    int parsed_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_number++;

        // 去除换行符
        line[strcspn(line, "\n\r")] = '\0';

        // 跳过空行和注释行
        trim_whitespace_inplace(line);
        if (strlen(line) == 0 || line[0] == '#') {
            continue;
        }

        // 解析配置行
        if (parse_config_line(line, categories) == 0) {
            parsed_count++;
        } else {
            fprintf(stderr, "警告: 配置文件第 %d 行格式错误: %s\n",
                    line_number, line);
        }
    }

    fclose(fp);

    if (parsed_count > 0) {
        printf("\033[32m【配置加载】\033[m成功解析 %d 个配置项\n", parsed_count);
        print_categories_summary(categories);
        return 0;
    } else {
        fprintf(stderr, "错误: 配置文件中没有有效的配置项\n");
        return -1;
    }
}

/**
 * 解析配置文件中的一行
 *
 * @param line 配置行内容
 * @param categories 时间分类结构指针
 * @return 0 成功，-1 失败
 */
static int parse_config_line(const char* line, TimeCategories* categories) {
    if (!line || !categories) return -1;

    char key[64];
    int value;

    // 解析 key=value 格式
    if (sscanf(line, "%63[^=]=%d", key, &value) != 2) {
        return -1;
    }

    // 去除key前后的空格
    trim_whitespace_inplace(key);

    // 验证值的合理性
    if (value < 0 || value > 30000) {  // 最大30秒
        fprintf(stderr, "警告: 配置值 %d 超出合理范围 (0-30000ms)\n", value);
        return -1;
    }

    // 设置对应的阈值
    if (strcasecmp(key, "excellent_max") == 0) {
        categories->excellent_max = value;
    } else if (strcasecmp(key, "good_max") == 0) {
        categories->good_max = value;
    } else if (strcasecmp(key, "normal_max") == 0) {
        categories->normal_max = value;
    } else if (strcasecmp(key, "general_max") == 0) {
        categories->general_max = value;
    } else if (strcasecmp(key, "poor_max") == 0) {
        categories->poor_max = value;
    } else if (strcasecmp(key, "severe_max") == 0) {
        categories->severe_max = value;
    } else if (strcasecmp(key, "suspect_threshold") == 0) {
        categories->suspect_threshold = value;
    } else {
        fprintf(stderr, "警告: 未知的配置键 '%s'\n", key);
        return -1;
    }

    return 0;
}

/**
 * 去除字符串前后的空白字符（就地修改）
 *
 * @param str 要处理的字符串
 */
static void trim_whitespace_inplace(char* str) {
    if (!str) return;

    // 去除前导空格
    char* start = str;
    while (isspace(*start)) start++;

    // 如果字符串全是空格
    if (*start == '\0') {
        *str = '\0';
        return;
    }

    // 移动字符串到开头
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }

    // 去除尾部空格
    char* end = str + strlen(str) - 1;
    while (end > str && isspace(*end)) {
        *end = '\0';
        end--;
    }
}

/**
 * 打印时间分类配置摘要
 *
 * @param categories 时间分类结构指针
 */
static void print_categories_summary(const TimeCategories* categories) {
    if (!categories) return;

    printf("\033[32m【时间分类配置】\033[0m\n");
    printf("  优秀:     ≤ %d ms\n", categories->excellent_max);
    printf("  良好:     ≤ %d ms\n", categories->good_max);
    printf("  正常:     ≤ %d ms\n", categories->normal_max);
    printf("  一般:     ≤ %d ms\n", categories->general_max);
    printf("  欠佳:     ≤ %d ms\n", categories->poor_max);
    printf("  严重:     ≤ %d ms\n", categories->severe_max);
    printf("  可疑:     > %d ms (触发重测)\n", categories->suspect_threshold);
    printf("  损坏:     IO错误或重测后仍超阈值\n");
    printf("\n");
}

/**
 * 将读取时间分类到对应类别
 *
 * @param categories 时间分类结构指针
 * @param time_ms 读取时间（毫秒）
 * @return 时间类别
 */
TimeCategoryType categorize_time(TimeCategories* categories, int time_ms) {
    if (!categories) return TIME_CATEGORY_DAMAGED;

    TimeCategoryType category;

    // 首先检查是否达到可疑标准 - 如果达到，应该触发重测
    if (time_ms >= categories->suspect_threshold) {
        category = TIME_CATEGORY_SUSPECT;  // 达到可疑标准，需要重测
    } else {
        // 未达到可疑标准，进行正常的6级分类
        if (time_ms <= categories->excellent_max) {
            category = TIME_CATEGORY_EXCELLENT;
        } else if (time_ms <= categories->good_max) {
            category = TIME_CATEGORY_GOOD;
        } else if (time_ms <= categories->normal_max) {
            category = TIME_CATEGORY_NORMAL;
        } else if (time_ms <= categories->general_max) {
            category = TIME_CATEGORY_GENERAL;
        } else if (time_ms <= categories->poor_max) {
            category = TIME_CATEGORY_POOR;
        } else if (time_ms <= categories->severe_max) {
            category = TIME_CATEGORY_SEVERE;
        } else {
            // 超过严重阈值但未达到可疑阈值，归为严重
            category = TIME_CATEGORY_SEVERE;
        }
    }

    // 更新统计信息
    categories->counts[category]++;
    categories->total_reads++;
    categories->total_time_ms += time_ms;

    // 更新最小/最大时间
    if (categories->total_reads == 1) {
        categories->min_time_ms = time_ms;
        categories->max_time_ms = time_ms;
    } else {
        if (time_ms < categories->min_time_ms) {
            categories->min_time_ms = time_ms;
        }
        if (time_ms > categories->max_time_ms) {
            categories->max_time_ms = time_ms;
        }
    }

    return category;
}

/**
 * 获取时间类别的名称
 *
 * @param type 时间类别类型
 * @return 类别名称字符串
 */
static const char* get_category_name(TimeCategoryType type) {
    switch (type) {
        case TIME_CATEGORY_EXCELLENT: return "优秀";
        case TIME_CATEGORY_GOOD: return "良好";
        case TIME_CATEGORY_NORMAL: return "正常";
        case TIME_CATEGORY_GENERAL: return "一般";
        case TIME_CATEGORY_POOR: return "欠佳";
        case TIME_CATEGORY_SEVERE: return "严重";
        case TIME_CATEGORY_SUSPECT: return "可疑";
        case TIME_CATEGORY_DAMAGED: return "损坏";
        default: return "未知";
    }
}

/**
 * 获取时间类别的颜色代码
 *
 * @param type 时间类别类型
 * @return ANSI 颜色代码字符串
 */
static const char* get_category_color(TimeCategoryType type) {
    switch (type) {
        case TIME_CATEGORY_EXCELLENT: return "\033[1;32m";  // 亮绿色
        case TIME_CATEGORY_GOOD: return "\033[32m";         // 绿色
        case TIME_CATEGORY_NORMAL: return "\033[36m";       // 青色
        case TIME_CATEGORY_GENERAL: return "\033[33m";      // 黄色
        case TIME_CATEGORY_POOR: return "\033[1;33m";       // 亮黄色
        case TIME_CATEGORY_SEVERE: return "\033[31m";       // 红色
        case TIME_CATEGORY_SUSPECT: return "\033[35m";      // 紫色
        case TIME_CATEGORY_DAMAGED: return "\033[1;31m";    // 亮红色
        default: return "\033[0m";                          // 默认颜色
    }
}

/**
 * 打印带颜色的时间类别信息
 *
 * @param type 时间类别类型
 * @param time_ms 时间（毫秒）
 */
void print_time_category(TimeCategoryType type, int time_ms) {
    const char* color = get_category_color(type);
    const char* name = get_category_name(type);

    printf("%s%s\033[0m (%d ms)", color, name, time_ms);
}

/**
 * 打印时间分类统计报告
 *
 * @param categories 时间分类结构指针
 */
void print_time_statistics(const TimeCategories* categories) {
    if (!categories || categories->total_reads == 0) {
        printf("\033[33m【统计报告】\033[m没有读取数据\n");
        return;
    }

    printf("\033[1;36m【时间分类统计报告】\033[0m\n");
    printf("总读取次数: %lu\n", categories->total_reads);
    printf("总耗时: %lu 毫秒\n", categories->total_time_ms);
    printf("平均时间: %.2f 毫秒\n",
           (double)categories->total_time_ms / categories->total_reads);
    printf("最短时间: %d 毫秒\n", categories->min_time_ms);
    printf("最长时间: %d 毫秒\n", categories->max_time_ms);
    printf("\n");

    printf("分类统计:\n");
    for (int i = 0; i < TIME_CATEGORY_COUNT; i++) {
        TimeCategoryType type = (TimeCategoryType)i;
        unsigned long count = categories->counts[i];
        double percentage = (double)count / categories->total_reads * 100;

        printf("  %s%-10s\033[0m: %8lu 次 (%6.2f%%)\n",
               get_category_color(type),
               get_category_name(type),
               count,
               percentage);
    }
    printf("\n");

    // 健康度评估
    double excellent_ratio = (double)categories->counts[TIME_CATEGORY_EXCELLENT] / categories->total_reads;
    double good_ratio = (double)categories->counts[TIME_CATEGORY_GOOD] / categories->total_reads;
    double normal_ratio = (double)categories->counts[TIME_CATEGORY_NORMAL] / categories->total_reads;
    double bad_ratio = (double)(categories->counts[TIME_CATEGORY_POOR] +
                                categories->counts[TIME_CATEGORY_SEVERE] +
                                categories->counts[TIME_CATEGORY_SUSPECT] +
                                categories->counts[TIME_CATEGORY_DAMAGED]) / categories->total_reads;

    printf("健康度评估:\n");
    if (excellent_ratio >= 0.8) {
        printf("  \033[1;32m健康状况: 优秀\033[0m - 设备性能良好\n");
    } else if (excellent_ratio + good_ratio >= 0.7) {
        printf("  \033[32m健康状况: 良好\033[0m - 设备性能正常\n");
    } else if (excellent_ratio + good_ratio + normal_ratio >= 0.6) {
        printf("  \033[36m健康状况: 正常\033[0m - 设备性能可接受\n");
    } else if (bad_ratio <= 0.1) {
        printf("  \033[33m健康状况: 一般\033[0m - 设备性能有所下降\n");
    } else if (bad_ratio <= 0.3) {
        printf("  \033[31m健康状况: 较差\033[0m - 建议备份数据\n");
    } else {
        printf("  \033[1;31m健康状况: 糟糕\033[0m - 强烈建议更换设备\n");
    }

    if (categories->counts[TIME_CATEGORY_DAMAGED] > 0) {
        printf("  \033[1;31m警告:\033[0m 发现 %lu 个损坏扇区，设备可能存在硬件故障\n",
               categories->counts[TIME_CATEGORY_DAMAGED]);
    }

    if (categories->counts[TIME_CATEGORY_SUSPECT] > 0) {
        printf("  \033[35m注意:\033[0m 发现 %lu 个可疑扇区，已触发重测机制\n",
               categories->counts[TIME_CATEGORY_SUSPECT]);
    }

    if (bad_ratio > 0.05) {
        printf("  \033[33m建议:\033[0m 发现 %.1f%% 的读取性能较差，请检查设备健康状况\n",
               bad_ratio * 100);
    }

    printf("\n");
}

/**
 * 保存时间分类配置到文件
 *
 * @param categories 时间分类结构指针
 * @param config_file 配置文件路径
 * @return 0 成功，-1 失败
 */
int save_time_categories_config(const TimeCategories* categories, const char* config_file) {
    if (!categories || !config_file) {
        fprintf(stderr, "错误: 参数为空\n");
        return -1;
    }

    FILE* fp = fopen(config_file, "w");
    if (!fp) {
        fprintf(stderr, "错误: 无法创建配置文件 '%s': %s\n",
                config_file, strerror(errno));
        return -1;
    }

    fprintf(fp, "# 时间分类配置文件\n");
    fprintf(fp, "# 由磁盘健康扫描工具自动生成\n");
    fprintf(fp, "# 所有时间单位为毫秒\n\n");

    fprintf(fp, "# 优秀: 响应时间极佳\n");
    fprintf(fp, "excellent_max=%d\n\n", categories->excellent_max);

    fprintf(fp, "# 良好: 响应时间很好\n");
    fprintf(fp, "good_max=%d\n\n", categories->good_max);

    fprintf(fp, "# 正常: 响应时间正常\n");
    fprintf(fp, "normal_max=%d\n\n", categories->normal_max);

    fprintf(fp, "# 一般: 响应时间开始变慢\n");
    fprintf(fp, "general_max=%d\n\n", categories->general_max);

    fprintf(fp, "# 欠佳: 响应时间较差\n");
    fprintf(fp, "poor_max=%d\n\n", categories->poor_max);

    fprintf(fp, "# 严重: 响应时间很差\n");
    fprintf(fp, "severe_max=%d\n\n", categories->severe_max);

    fprintf(fp, "# 可疑: 超过此阈值将触发重测\n");
    fprintf(fp, "suspect_threshold=%d\n\n", categories->suspect_threshold);

    fprintf(fp, "# 损坏: 仅当发生IO错误或重测后仍超阈值时标记\n");

    fclose(fp);

    printf("\033[32m【配置保存】\033[m配置已保存到: %s\n", config_file);
    return 0;
}

/**
 * 检查时间分类配置的合理性
 *
 * @param categories 时间分类结构指针
 * @return 0 合理，-1 不合理
 */
int validate_time_categories(const TimeCategories* categories) {
    if (!categories) return -1;

    // 检查前6级阈值是否递增
    if (categories->excellent_max >= categories->good_max ||
        categories->good_max >= categories->normal_max ||
        categories->normal_max >= categories->general_max ||
        categories->general_max >= categories->poor_max ||
        categories->poor_max >= categories->severe_max) {

        fprintf(stderr, "错误: 前6级时间分类阈值必须递增\n");
        fprintf(stderr, "当前设置: 优秀(%d) < 良好(%d) < 正常(%d) < 一般(%d) < 欠佳(%d) < 严重(%d)\n",
                categories->excellent_max, categories->good_max, categories->normal_max,
                categories->general_max, categories->poor_max, categories->severe_max);
        return -1;
    }

    // 检查可疑阈值必须大于等于正常上限
    if (categories->suspect_threshold < categories->normal_max) {
        fprintf(stderr, "错误: 可疑阈值(%d)必须大于等于正常上限(%d)\n",
                categories->suspect_threshold, categories->normal_max);
        return -1;
    }

    // 检查阈值范围
    if (categories->excellent_max <= 0 || categories->suspect_threshold > 30000) {
        fprintf(stderr, "错误: 时间阈值超出合理范围 (1-30000ms)\n");
        return -1;
    }

    return 0;
}

/**
 * 判断是否需要对扇区进行重测
 *
 * @param categories 时间分类结构指针
 * @param time_ms 响应时间（毫秒）
 * @return 1 需要重测，0 不需要重测
 */
int should_retest_sector(const TimeCategories* categories, int time_ms) {
    if (!categories) return 0;
    return time_ms >= categories->suspect_threshold;
}

/**
 * 时间分类（不更新统计信息）
 *
 * @param categories 时间分类结构指针
 * @param time_ms 读取时间（毫秒）
 * @return 时间类别
 */
TimeCategoryType categorize_time_without_stats(const TimeCategories* categories, int time_ms) {
    if (!categories) return TIME_CATEGORY_DAMAGED;

    // 正常的6级分类
    if (time_ms <= categories->excellent_max) {
        return TIME_CATEGORY_EXCELLENT;
    } else if (time_ms <= categories->good_max) {
        return TIME_CATEGORY_GOOD;
    } else if (time_ms <= categories->normal_max) {
        return TIME_CATEGORY_NORMAL;
    } else if (time_ms <= categories->general_max) {
        return TIME_CATEGORY_GENERAL;
    } else if (time_ms <= categories->poor_max) {
        return TIME_CATEGORY_POOR;
    } else if (time_ms <= categories->severe_max) {
        return TIME_CATEGORY_SEVERE;
    } else {
        // 超过严重阈值的情况，归为严重级别
        // 注意：可疑级别主要通过重测流程来确定
        return TIME_CATEGORY_SEVERE;
    }
}

/**
 * 带重测功能的时间分类
 *
 * @param categories 时间分类结构指针
 * @param sector 扇区号
 * @param time_ms 初始响应时间（毫秒）
 * @param retest_result 重测结果（可选）
 * @return 最终时间类别
 */
TimeCategoryType categorize_time_with_retest(TimeCategories* categories, unsigned long sector __attribute__((unused)),
                                           int time_ms, RetestResult* retest_result) {
    if (!categories) return TIME_CATEGORY_DAMAGED;

    // 如果不需要重测，直接使用普通分类
    if (!should_retest_sector(categories, time_ms)) {
        return categorize_time(categories, time_ms);
    }

    // 需要重测的情况
    if (retest_result) {
        retest_result->original_time = time_ms;
        // 这里应该由调用者执行实际的重测操作
        // perform_sector_retest(sector, device_path, retest_result);
        // return process_retest_result(categories, retest_result);
    }

    // 暂时标记为可疑，等待重测
    categories->counts[TIME_CATEGORY_SUSPECT]++;
    categories->total_reads++;
    categories->total_time_ms += time_ms;

    return TIME_CATEGORY_SUSPECT;
}

/**
 * 获取分类名称（对外接口）
 *
 * @param type 时间类别类型
 * @return 类别名称字符串
 */
const char* get_category_name_str(TimeCategoryType type) {
    return get_category_name(type);
}

/**
 * 获取分类颜色（对外接口）
 *
 * @param type 时间类别类型
 * @return 颜色代码字符串
 */
const char* get_category_color_str(TimeCategoryType type) {
    return get_category_color(type);
}

/**
 * 获取设备类型名称（对外接口）
 *
 * @param device_type 设备类型
 * @return 设备类型名称字符串
 */
const char* get_device_type_name(DeviceType device_type) {
    switch (device_type) {
        case DEVICE_TYPE_NVME_SSD: return "NVMe SSD";
        case DEVICE_TYPE_SATA_SSD: return "SATA SSD";
        case DEVICE_TYPE_HDD: return "机械硬盘";
        case DEVICE_TYPE_USB_STORAGE: return "USB 存储设备";
        case DEVICE_TYPE_UNKNOWN_SSD: return "未知 SSD";
        case DEVICE_TYPE_UNKNOWN:
        default: return "未知设备";
    }
}

/**
 * 获取设备类型的简单名称（用于初始化时显示）
 *
 * @param device_type 设备类型枚举
 * @return 设备类型名称字符串
 */
static const char* get_simple_device_type_name(DeviceType device_type) {
    switch (device_type) {
        case DEVICE_TYPE_NVME_SSD: return "NVMe SSD";
        case DEVICE_TYPE_SATA_SSD: return "SATA SSD";
        case DEVICE_TYPE_UNKNOWN_SSD: return "未知 SSD";
        case DEVICE_TYPE_HDD: return "机械硬盘";
        case DEVICE_TYPE_USB_STORAGE: return "USB 存储设备";
        case DEVICE_TYPE_UNKNOWN:
        default: return "未知设备";
    }
}
