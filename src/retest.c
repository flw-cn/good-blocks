// retest.c - 可疑扇区重测功能实现
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#include "retest.h"

// 内部函数声明
static int calculate_retest_average(const RetestResult* result);

/**
 * 初始化重测配置
 *
 * @param config 重测配置结构指针
 */
void init_retest_config(RetestConfig* config) {
    if (!config) return;

    config->max_retests = 3;        // 默认重测3次
    config->retest_interval_ms = 100;  // 默认间隔100ms
    config->silent_mode = 1;        // 默认静默模式（不影响进度条）
}

/**
 * 设置重测配置
 *
 * @param config 重测配置结构指针
 * @param max_retests 最大重测次数
 * @param interval_ms 重测间隔（毫秒）
 */
void set_retest_config(RetestConfig* config, int max_retests, int interval_ms) {
    if (!config) return;

    if (max_retests > 0 && max_retests <= 10) {
        config->max_retests = max_retests;
    }

    if (interval_ms >= 0 && interval_ms <= 5000) {
        config->retest_interval_ms = interval_ms;
    }
}

/**
 * 设置重测静默模式
 *
 * @param config 重测配置结构指针
 * @param silent 静默模式：1=静默，0=显示详细信息
 */
void set_retest_silent_mode(RetestConfig* config, int silent) {
    if (!config) return;
    config->silent_mode = silent ? 1 : 0;
}

/**
 * 对指定扇区进行重测
 *
 * @param sector 扇区号
 * @param device_path 设备路径
 * @param config 重测配置
 * @param result 重测结果存储
 * @return 0 成功，-1 失败
 */
int perform_sector_retest(unsigned long sector, const char* device_path,
                         const RetestConfig* config, RetestResult* result) {
    if (!device_path || !result) return -1;

    // 使用默认配置（如果未提供）
    RetestConfig default_config;
    if (!config) {
        init_retest_config(&default_config);
        config = &default_config;
    }

    // 初始化结果结构
    result->sector = sector;
    result->retest_count = 0;
    result->average_time = 0;
    result->final_category = TIME_CATEGORY_DAMAGED;

    int fd = open(device_path, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        fprintf(stderr, "重测失败: 无法打开设备 %s: %s\n", device_path, strerror(errno));
        return -1;
    }

    // 计算扇区偏移
    off_t offset = (off_t)sector * 512;

    // 进行重测
    char buffer[512] __attribute__((aligned(512)));

    for (int i = 0; i < config->max_retests && result->retest_count < 5; i++) {
        struct timespec start, end;

        // 定位到扇区
        if (lseek(fd, offset, SEEK_SET) == -1) {
            fprintf(stderr, "重测失败: 无法定位到扇区 %lu: %s\n", sector, strerror(errno));
            close(fd);
            return -1;
        }

        // 记录开始时间
        clock_gettime(CLOCK_MONOTONIC, &start);

        // 读取扇区
        ssize_t bytes_read = read(fd, buffer, 512);

        // 记录结束时间
        clock_gettime(CLOCK_MONOTONIC, &end);

        if (bytes_read == 512) {
            // 计算响应时间
            long elapsed_ms = (end.tv_sec - start.tv_sec) * 1000 +
                             (end.tv_nsec - start.tv_nsec) / 1000000;

            result->retest_times[result->retest_count] = (int)elapsed_ms;
            result->retest_count++;

            if (!config->silent_mode) {
                printf("  重测 #%d: %ld ms\n", result->retest_count, elapsed_ms);
            }
        } else {
            // IO错误，直接标记为损坏
            close(fd);
            result->final_category = TIME_CATEGORY_DAMAGED;
            if (!config->silent_mode) {
                printf("  重测检测到IO错误，确认为坏道\n");
            }
            return 0;
        }

        // 重测间隔
        if (config->retest_interval_ms > 0 && i < config->max_retests - 1) {
            usleep(config->retest_interval_ms * 1000);
        }
    }

    close(fd);

    // 计算平均值（去除最大值和最小值）
    if (result->retest_count >= 3) {
        result->average_time = calculate_retest_average(result);
    } else if (result->retest_count > 0) {
        // 重测次数不足，使用简单平均
        int sum = 0;
        for (int i = 0; i < result->retest_count; i++) {
            sum += result->retest_times[i];
        }
        result->average_time = sum / result->retest_count;
    }

    // 如果有重测结果，设置最终分类为正常（不是损坏）
    if (result->retest_count > 0) {
        result->final_category = TIME_CATEGORY_NORMAL;  // 重测成功表示不是真正的坏道
    }

    if (!config->silent_mode) {
        printf("  重测完成: %d次测试, 平均时间 %d ms\n",
               result->retest_count, result->average_time);
    }

    return 0;
}

/**
 * 处理重测结果并更新分类
 *
 * @param categories 时间分类结构指针
 * @param result 重测结果
 * @return 最终分类结果
 */
TimeCategoryType process_retest_result(TimeCategories* categories, RetestResult* result) {
    if (!categories || !result) return TIME_CATEGORY_DAMAGED;

    if (result->final_category == TIME_CATEGORY_DAMAGED) {
        // 已经确认为损坏（IO错误）
        categories->counts[TIME_CATEGORY_DAMAGED]++;
        return TIME_CATEGORY_DAMAGED;
    }

    // 根据平均时间重新分类
    TimeCategoryType new_category = categorize_time_without_stats(categories, result->average_time);

    // 如果重测后仍然超过可疑阈值，根据情况处理
    if (result->average_time >= categories->suspect_threshold) {
        // 重测后仍然很慢，可能是真的有问题，但不是完全损坏
        // 根据具体时间进行分类
        if (result->average_time > categories->severe_max * 2) {
            new_category = TIME_CATEGORY_DAMAGED;  // 极慢，视为坏道
        }
        // 否则保持基于时间的分类
    }

    result->final_category = new_category;
    categories->counts[new_category]++;

    return new_category;
}

/**
 * 计算重测结果的平均值（去除最大值和最小值）
 *
 * @param result 重测结果
 * @return 平均时间
 */
static int calculate_retest_average(const RetestResult* result) {
    if (!result || result->retest_count < 3) return 0;

    int times[5];
    for (int i = 0; i < result->retest_count; i++) {
        times[i] = result->retest_times[i];
    }

    // 简单排序
    for (int i = 0; i < result->retest_count - 1; i++) {
        for (int j = i + 1; j < result->retest_count; j++) {
            if (times[i] > times[j]) {
                int temp = times[i];
                times[i] = times[j];
                times[j] = temp;
            }
        }
    }

    // 去除最大值和最小值，计算中间值的平均
    int sum = 0;
    int count = 0;
    for (int i = 1; i < result->retest_count - 1; i++) {
        sum += times[i];
        count++;
    }

    return count > 0 ? sum / count : times[0];
}
