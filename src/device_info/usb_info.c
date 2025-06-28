// src/device_info/usb_info.c - USB 存储设备信息收集模块

#include "usb_info.h"
#include "generic_info.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// 内部函数声明
static void finalize_usb_info(DeviceInfo* info);
static int detect_usb_device_type(DeviceInfo* info);

/**
 * USB 存储设备信息收集主函数
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 */
int collect_usb_info(DeviceInfo* info) {
    if (!info) return -1;

    int success_count = 0;
    int total_attempts = 0;

    printf("\033[34m【USB 信息】\033[m开始收集 USB 存储设备信息...\n");

    // 收集通用信息
    total_attempts++;
    if (collect_generic_info(info) == 0) {
        success_count++;
        printf("\033[34m【USB 信息】\033[m通用信息收集成功\n");
    } else {
        printf("\033[33m【USB 警告】\033[m通用信息收集失败\n");
    }

    // 设置 USB 设备特定属性
    info->bus_type = BUS_TYPE_USB;

    // 检测 USB 设备类型
    detect_usb_device_type(info);

#if USE_SMARTCTL
    // 尝试使用 smartctl（某些 USB 设备支持）
    total_attempts++;
    printf("\033[34m【USB 信息】\033[m尝试使用 smartctl 收集信息...\n");
    if (collect_smartctl_info(info) == 0) {
        success_count++;
        printf("\033[34m【USB 信息】\033[m smartctl 信息收集成功\n");
    } else {
        printf("\033[33m【USB 警告】\033[m smartctl 信息收集失败（正常情况）\n");
    }
#endif

    // 最终化设备信息
    finalize_usb_info(info);

    printf("\033[34m【USB 信息】\033[m信息收集完成，成功 %d/%d 项\n", success_count, total_attempts);

    return (success_count > 0) ? 0 : -1;
}

/**
 * 检测 USB 设备类型
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 *
 * USB 存储设备可能是机械硬盘（移动硬盘）或 SSD/Flash（U盘、移动SSD）
 */
static int detect_usb_device_type(DeviceInfo* info) {
    printf("\033[34m【USB 分类】\033[m开始 USB 设备类型检测...\n");

    // 方法1：根据 rotational 属性判断
    if (info->is_rotational == 1) {
        // 旋转设备 - USB 移动硬盘
        info->device_type = DEVICE_TYPE_HDD;
        printf("\033[34m【USB 分类】\033[m根据 rotational=1 判断为 USB 移动硬盘\n");
        return 0;
    } else if (info->is_rotational == 0) {
        // 非旋转设备 - USB SSD 或 Flash
        info->device_type = DEVICE_TYPE_USB_STORAGE;
        printf("\033[34m【USB 分类】\033[m根据 rotational=0 判断为 USB 固态存储\n");
        return 0;
    }

    // 方法2：根据容量大小推断
    if (info->capacity_gb > 0) {
        if (info->capacity_gb >= 500) {
            // 大容量设备，可能是移动硬盘
            info->device_type = DEVICE_TYPE_HDD;
            info->is_rotational = 1;  // 推断为机械硬盘
            printf("\033[34m【USB 分类】\033[m根据大容量 %.2f GB 推断为 USB 移动硬盘\n", info->capacity_gb);
        } else {
            // 小容量设备，可能是 U盘 或小容量 SSD
            info->device_type = DEVICE_TYPE_USB_STORAGE;
            info->is_rotational = 0;  // 推断为固态存储
            printf("\033[34m【USB 分类】\033[m根据容量 %.2f GB 推断为 USB 固态存储\n", info->capacity_gb);
        }
        return 0;
    }

    // 方法3：根据型号信息推断
    if (strlen(info->model) > 0) {
        char model_lower[MAX_MODEL_LEN];
        strncpy(model_lower, info->model, sizeof(model_lower) - 1);
        model_lower[sizeof(model_lower) - 1] = '\0';

        // 转换为小写
        for (int i = 0; model_lower[i]; i++) {
            model_lower[i] = tolower(model_lower[i]);
        }

        // 检查关键词
        if (strstr(model_lower, "flash") || strstr(model_lower, "stick") ||
            strstr(model_lower, "drive") || strstr(model_lower, "ssd")) {
            info->device_type = DEVICE_TYPE_USB_STORAGE;
            info->is_rotational = 0;
            printf("\033[34m【USB 分类】\033[m根据型号 '%s' 推断为固态存储\n", info->model);
        } else if (strstr(model_lower, "disk") || strstr(model_lower, "hdd")) {
            info->device_type = DEVICE_TYPE_HDD;
            info->is_rotational = 1;
            printf("\033[34m【USB 分类】\033[m根据型号 '%s' 推断为机械硬盘\n", info->model);
        } else {
            // 默认假设是固态存储（现代 USB 设备多数是 Flash）
            info->device_type = DEVICE_TYPE_USB_STORAGE;
            info->is_rotational = 0;
            printf("\033[34m【USB 分类】\033[m无法确定，默认推断为固态存储\n");
        }
        return 0;
    }

    // 默认情况：假设是固态存储
    info->device_type = DEVICE_TYPE_USB_STORAGE;
    info->is_rotational = 0;
    printf("\033[33m【USB 警告】\033[m信息不足，默认假设为固态存储\n");

    return 0;
}

/**
 * 最终化 USB 设备信息
 *
 * @param info 设备信息结构指针
 *
 * 设置合理的默认值和进行数据验证
 */
static void finalize_usb_info(DeviceInfo* info) {
    // 确保总线类型正确
    info->bus_type = BUS_TYPE_USB;

    // 设置合理的默认转速
    if (info->device_type == DEVICE_TYPE_HDD && info->rotation_rate_rpm == 0) {
        // USB 移动硬盘通常是 5400 RPM（为了降低功耗和发热）
        info->rotation_rate_rpm = 5400;
        printf("\033[34m【USB 最终】\033[m设置 USB 移动硬盘默认转速: 5400 RPM\n");
    } else if (info->device_type == DEVICE_TYPE_USB_STORAGE) {
        // 确保固态存储设备转速为 0
        info->rotation_rate_rpm = 0;
        info->is_rotational = 0;
    }

    // 设置合理的最优 I/O 大小
    if (info->optimal_io_size == 0) {
        if (info->device_type == DEVICE_TYPE_USB_STORAGE) {
            // USB Flash/SSD 设备
            info->optimal_io_size = 4096;  // 4KB 通常是较好的选择
        } else {
            // USB 移动硬盘
            info->optimal_io_size = (info->physical_block_size > 0) ? info->physical_block_size : 4096;
        }
        printf("\033[34m【USB 最终】\033[m设置最优 I/O 大小: %u 字节\n", info->optimal_io_size);
    }

    // 确保基本块大小设置
    if (info->logical_block_size == 0) {
        info->logical_block_size = 512;
    }

    if (info->physical_block_size == 0) {
        // USB 设备通常使用 4K 物理块
        info->physical_block_size = 4096;
    }

    // 重新计算容量（如果需要）
    if (info->capacity_gb == 0 && info->total_sectors > 0) {
        double total_bytes = (double)info->total_sectors * 512;
        info->capacity_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
    }

    printf("\033[34m【USB 最终】\033[m设备信息最终化完成\n");
    printf("\033[34m【USB 最终】\033[m - 设备类型: %s\n", get_device_type_str(info));
    printf("\033[34m【USB 最终】\033[m - 接口类型: %s\n", get_bus_type_str(info->bus_type));
    printf("\033[34m【USB 最终】\033[m - 是否旋转: %s\n",
           info->is_rotational == 1 ? "是" : (info->is_rotational == 0 ? "否" : "未知"));
    if (info->rotation_rate_rpm > 0) {
        printf("\033[34m【USB 最终】\033[m - 转速: %d RPM\n", info->rotation_rate_rpm);
    }
    if (info->capacity_gb > 0) {
        printf("\033[34m【USB 最终】\033[m - 容量: %.2f GB\n", info->capacity_gb);
    }
    printf("\033[34m【USB 最终】\033[m - 最优 I/O 大小: %u 字节\n", info->optimal_io_size);
}
