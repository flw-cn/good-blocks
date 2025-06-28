// time_categories.h
#ifndef TIME_CATEGORIES_H
#define TIME_CATEGORIES_H

#include "device_info/device_info.h"

// 时间分类枚举 - 基于响应时间的8级分类
typedef enum {
    TIME_CATEGORY_EXCELLENT = 0,    // 优秀 - 响应时间极佳
    TIME_CATEGORY_GOOD,             // 良好 - 响应时间很好
    TIME_CATEGORY_NORMAL,           // 正常 - 响应时间正常
    TIME_CATEGORY_GENERAL,          // 一般 - 响应时间开始变慢
    TIME_CATEGORY_POOR,             // 欠佳 - 响应时间较差
    TIME_CATEGORY_SEVERE,           // 严重 - 响应时间很差
    TIME_CATEGORY_SUSPECT,          // 可疑 - 需要重测确认
    TIME_CATEGORY_DAMAGED,          // 损坏 - IO错误，真正的坏道
    TIME_CATEGORY_COUNT
} TimeCategoryType;

// 时间分类配置结构
typedef struct {
    // 响应时间阈值配置（毫秒）
    int excellent_max;      // 优秀上限
    int good_max;           // 良好上限
    int normal_max;         // 正常上限
    int general_max;        // 一般上限
    int poor_max;           // 欠佳上限
    int severe_max;         // 严重上限
    int suspect_threshold;  // 可疑阈值（触发重测）

    // 统计信息
    unsigned long counts[TIME_CATEGORY_COUNT];  // 各类别计数
    unsigned long total_reads;                  // 总读取次数
    unsigned long total_time_ms;                // 总耗时（毫秒）
    int min_time_ms;                           // 最短时间
    int max_time_ms;                           // 最长时间
} TimeCategories;

// 重测结果结构
typedef struct {
    unsigned long sector;           // 扇区号
    int original_time;             // 初始响应时间
    int retest_times[5];           // 重测结果数组
    int retest_count;              // 实际重测次数
    int average_time;              // 去除最值后的平均时间
    TimeCategoryType final_category;  // 最终分类结果
} RetestResult;

// 函数声明
void initialize_time_categories(TimeCategories* categories, DeviceType device_type);
int load_time_categories_config(TimeCategories* categories, const char* config_file);
int save_time_categories_config(const TimeCategories* categories, const char* config_file);
int validate_time_categories(const TimeCategories* categories);

// 时间分类核心函数
TimeCategoryType categorize_time(TimeCategories* categories, int time_ms);                    // 主分类函数：先检查可疑，再进行6级分类，更新统计
TimeCategoryType categorize_time_without_stats(const TimeCategories* categories, int time_ms); // 纯分类函数：仅进行6级分类，不更新统计
TimeCategoryType categorize_time_with_retest(TimeCategories* categories, unsigned long sector, int time_ms, RetestResult* retest_result); // 重测分类函数：处理重测流程

// 重测判断函数 (重测具体实现在 retest.c 中)
int should_retest_sector(const TimeCategories* categories, int time_ms);

// 输出和统计函数
void print_time_category(TimeCategoryType type, int time_ms);
void print_time_statistics(const TimeCategories* categories);
void print_time_categories_config(const TimeCategories* categories);
const char* get_category_name_str(TimeCategoryType type);
const char* get_category_color_str(TimeCategoryType type);

#endif // TIME_CATEGORIES_H
