// src/device_info/sata_info.c - SATA/PATA 设备信息收集模块

#include "sata_info.h"
#include "generic_info.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// 内部函数声明
static int classify_sata_device(DeviceInfo* info);
static void finalize_sata_info(DeviceInfo* info);
static int determine_bus_type_from_udev(DeviceInfo* info);

/**
 * SATA/PATA 设备信息收集主函数
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 */
int collect_sata_info(DeviceInfo* info) {
    if (!info) return -1;

    int success_count = 0;
    int total_attempts = 0;

    printf("\033[35m【SATA 信息】\033[m开始收集 SATA/PATA 设备信息...\n");

    // 收集通用信息（sysfs, udevadm）
    total_attempts++;
    if (collect_generic_info(info) == 0) {
        success_count++;
        printf("\033[35m【SATA 信息】\033[m通用信息收集成功\n");
    } else {
        printf("\033[33m【SATA 警告】\033[m通用信息收集失败\n");
    }

    // 进一步确定总线类型
    determine_bus_type_from_udev(info);

#if USE_SMARTCTL
    // 尝试使用 smartctl 获取详细信息
    total_attempts++;
    printf("\033[35m【SATA 信息】\033[m尝试使用 smartctl 收集信息...\n");
    if (collect_smartctl_info(info) == 0) {
        success_count++;
        printf("\033[35m【SATA 信息】\033[m smartctl 信息收集成功\n");
    } else {
        printf("\033[33m【SATA 警告】\033[m smartctl 信息收集失败\n");
    }
#endif

    // 分类设备类型（HDD vs SSD）
    classify_sata_device(info);

    // 最终化设备信息
    finalize_sata_info(info);

    printf("\033[35m【SATA 信息】\033[m信息收集完成，成功 %d/%d 项\n", success_count, total_attempts);

    return (success_count > 0) ? 0 : -1;
}

/**
 * 从 udev 信息进一步确定总线类型
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 */
static int determine_bus_type_from_udev(DeviceInfo* info) {
    // 如果总线类型已经确定，跳过
    if (info->bus_type != BUS_TYPE_UNKNOWN) {
        return 0;
    }

    // 根据设备名进行推断
    if (strncmp(info->main_dev_name, "sd", 2) == 0) {
        // sd 设备在现代系统中通常是 SATA，但也可能是 SCSI 或 USB
        // 这里需要通过其他信息进一步判断
        info->bus_type = BUS_TYPE_SATA;  // 默认假设是 SATA
        printf("\033[35m【SATA 推断】\033[m设备名 '%s' 推断为 SATA 接口\n", info->main_dev_name);
    } else if (strncmp(info->main_dev_name, "hd", 2) == 0) {
        // hd 设备通常是 PATA
        info->bus_type = BUS_TYPE_PATA;
        printf("\033[35m【SATA 推断】\033[m设备名 '%s' 推断为 PATA 接口\n", info->main_dev_name);
    } else {
        printf("\033[33m【SATA 警告】\033[m无法从设备名 '%s' 推断总线类型\n", info->main_dev_name);
        return -1;
    }

    return 0;
}

/**
 * 设备类型判断和分类
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 *
 * 根据 rotational 属性、转速信息等判断是 HDD 还是 SSD
 */
static int classify_sata_device(DeviceInfo* info) {
    printf("\033[35m【SATA 分类】\033[m开始设备类型分类...\n");

    // 方法1：根据 rotational 属性判断
    if (info->is_rotational == 0) {
        // 非旋转设备，是 SSD
        if (info->bus_type == BUS_TYPE_SATA || info->bus_type == BUS_TYPE_ATA) {
            info->device_type = DEVICE_TYPE_SATA_SSD;
            printf("\033[35m【SATA 分类】\033[m根据 rotational=0 判断为 SATA SSD\n");
        } else {
            info->device_type = DEVICE_TYPE_UNKNOWN_SSD;
            printf("\033[35m【SATA 分类】\033[m根据 rotational=0 判断为 SSD（未知接口）\n");
        }
    } else if (info->is_rotational == 1) {
        // 旋转设备，是 HDD
        info->device_type = DEVICE_TYPE_HDD;
        printf("\033[35m【SATA 分类】\033[m根据 rotational=1 判断为机械硬盘\n");
    } else {
        // 方法2：根据转速信息判断
        if (info->rotation_rate_rpm > 0) {
            // 有转速信息，肯定是 HDD
            info->device_type = DEVICE_TYPE_HDD;
            info->is_rotational = 1;
            printf("\033[35m【SATA 分类】\033[m根据转速 %d RPM 判断为机械硬盘\n", info->rotation_rate_rpm);
        } else {
            // 方法3：根据型号信息推断
            if (strlen(info->model) > 0) {
                char model_lower[MAX_MODEL_LEN];
                strncpy(model_lower, info->model, sizeof(model_lower) - 1);
                model_lower[sizeof(model_lower) - 1] = '\0';

                // 转换为小写进行匹配
                for (int i = 0; model_lower[i]; i++) {
                    model_lower[i] = tolower(model_lower[i]);
                }

                // 检查型号中是否包含 SSD 相关关键词
                if (strstr(model_lower, "ssd") || strstr(model_lower, "solid state") ||
                    strstr(model_lower, "nvme") || strstr(model_lower, "flash")) {
                    info->device_type = DEVICE_TYPE_SATA_SSD;
                    info->is_rotational = 0;
                    printf("\033[35m【SATA 分类】\033[m根据型号 '%s' 推断为 SSD\n", info->model);
                } else {
                    // 无法确定，保持 UNKNOWN，但倾向于认为是 HDD
                    info->device_type = DEVICE_TYPE_UNKNOWN;
                    printf("\033[33m【SATA 警告】\033[m无法确定设备类型，型号: %s\n", info->model);
                }
            } else {
                // 完全无法确定
                info->device_type = DEVICE_TYPE_UNKNOWN;
                printf("\033[33m【SATA 警告】\033[m无法确定设备类型，信息不足\n");
            }
        }
    }

    return 0;
}

/**
 * 最终化 SATA 设备信息
 *
 * @param info 设备信息结构指针
 *
 * 设置合理的默认值和进行最后的数据验证
 */
static void finalize_sata_info(DeviceInfo* info) {
    // 确保总线类型正确设置
    if (info->bus_type == BUS_TYPE_UNKNOWN) {
        // 根据设备名进行最后的推断
        if (strncmp(info->main_dev_name, "sd", 2) == 0) {
            info->bus_type = BUS_TYPE_SATA;
        } else if (strncmp(info->main_dev_name, "hd", 2) == 0) {
            info->bus_type = BUS_TYPE_PATA;
        }
    }

    // 为 HDD 设置默认转速（如果没有从 smartctl 获取到）
    if (info->device_type == DEVICE_TYPE_HDD && info->rotation_rate_rpm == 0) {
        // 根据常见情况设置默认转速
        if (info->capacity_gb > 0) {
            if (info->capacity_gb >= 4000) {  // 4TB 以上通常是 7200 RPM
                info->rotation_rate_rpm = 7200;
            } else if (info->capacity_gb >= 1000) {  // 1TB-4TB 可能是 7200 或 5400
                info->rotation_rate_rpm = 7200;  // 默认 7200
            } else {  // 小容量可能是 5400
                info->rotation_rate_rpm = 5400;
            }
        } else {
            info->rotation_rate_rpm = 7200;  // 完全无信息时默认 7200
        }
        printf("\033[35m【SATA 最终】\033[m设置默认转速: %d RPM\n", info->rotation_rate_rpm);
    }

    // 为 SSD 确保 rotational 和 rpm 正确
    if (info->device_type == DEVICE_TYPE_SATA_SSD || info->device_type == DEVICE_TYPE_UNKNOWN_SSD) {
        info->is_rotational = 0;
        info->rotation_rate_rpm = 0;
    }

    // 设置合理的最优 I/O 大小
    if (info->optimal_io_size == 0) {
        if (info->device_type == DEVICE_TYPE_SATA_SSD || info->device_type == DEVICE_TYPE_UNKNOWN_SSD) {
            // SSD 通常 4KB 对齐性能更好
            info->optimal_io_size = 4096;
        } else if (info->device_type == DEVICE_TYPE_HDD) {
            // HDD 通常物理扇区是 4KB
            info->optimal_io_size = (info->physical_block_size > 0) ? info->physical_block_size : 4096;
        } else {
            // 未知类型，使用保守值
            info->optimal_io_size = 4096;
        }
        printf("\033[35m【SATA 最终】\033[m设置最优 I/O 大小: %u 字节\n", info->optimal_io_size);
    }

    // 数据一致性检查
    if (info->logical_block_size == 0) {
        info->logical_block_size = 512;  // 默认逻辑扇区大小
    }

    if (info->physical_block_size == 0) {
        // 现代大容量硬盘通常是 4K 物理扇区
        if (info->device_type == DEVICE_TYPE_HDD && info->capacity_gb > 500) {
            info->physical_block_size = 4096;
        } else {
            info->physical_block_size = info->logical_block_size;
        }
    }

    // 重新计算容量（如果需要）
    if (info->capacity_gb == 0 && info->total_sectors > 0) {
        double total_bytes = (double)info->total_sectors * 512;  // total_sectors 总是以 512 字节为单位
        info->capacity_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
    }

    printf("\033[35m【SATA 最终】\033[m设备信息最终化完成\n");
    printf("\033[35m【SATA 最终】\033[m - 设备类型: %s\n", get_device_type_str(info));
    printf("\033[35m【SATA 最终】\033[m - 接口类型: %s\n", get_bus_type_str(info->bus_type));
    printf("\033[35m【SATA 最终】\033[m - 是否旋转: %s\n",
           info->is_rotational == 1 ? "是" : (info->is_rotational == 0 ? "否" : "未知"));
    printf("\033[35m【SATA 最终】\033[m - 逻辑块大小: %u 字节\n", info->logical_block_size);
    printf("\033[35m【SATA 最终】\033[m - 物理块大小: %u 字节\n", info->physical_block_size);
    printf("\033[35m【SATA 最终】\033[m - 最优 I/O 大小: %u 字节\n", info->optimal_io_size);
    if (info->rotation_rate_rpm > 0) {
        printf("\033[35m【SATA 最终】\033[m - 转速: %d RPM\n", info->rotation_rate_rpm);
    }
    if (info->capacity_gb > 0) {
        printf("\033[35m【SATA 最终】\033[m - 容量: %.2f GB\n", info->capacity_gb);
    }
}
