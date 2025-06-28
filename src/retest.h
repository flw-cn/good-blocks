// retest.h - 可疑扇区重测功能
#ifndef RETEST_H
#define RETEST_H

#include "time_categories.h"

// 重测配置
typedef struct {
    int max_retests;        // 最大重测次数（默认3-5次）
    int retest_interval_ms; // 重测间隔（毫秒，默认100ms）
    int silent_mode;        // 静默模式：1=不输出重测信息，0=输出详细信息
} RetestConfig;

// 重测相关函数
int perform_sector_retest(unsigned long sector, const char* device_path,
                         const RetestConfig* config, RetestResult* result);
TimeCategoryType process_retest_result(TimeCategories* categories, RetestResult* result);

// 重测配置管理
void init_retest_config(RetestConfig* config);
void set_retest_config(RetestConfig* config, int max_retests, int interval_ms);
void set_retest_silent_mode(RetestConfig* config, int silent);

#endif // RETEST_H
