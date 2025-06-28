// src/device_info/device_info.c - 设备信息收集抽象层/调用入口

#include "device_info.h"
#include "sata_info.h"
#include "nvme_info.h"
#include "usb_info.h"
#include "generic_info.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

// 内部函数声明
static void finalize_device_info(DeviceInfo* info);
static int determine_device_type_from_name(DeviceInfo* info);
static void apply_device_specific_defaults(DeviceInfo* info);

/**
 * 初始化设备信息结构
 *
 * @param info 设备信息结构指针
 * @param dev_path 设备路径
 */
void initialize_device_info(DeviceInfo* info, const char* dev_path) {
    if (!info || !dev_path) return;

    // 清零整个结构
    memset(info, 0, sizeof(DeviceInfo));

    // 设置设备路径
    strncpy(info->dev_path, dev_path, sizeof(info->dev_path) - 1);
    info->dev_path[sizeof(info->dev_path) - 1] = '\0';

    // 设置默认值
    info->device_type = DEVICE_TYPE_UNKNOWN;
    info->bus_type = BUS_TYPE_UNKNOWN;
    info->is_rotational = -1;  // -1 表示未知
    info->rpm = 0;
    info->rotation_rate_rpm = 0;
    info->info_collection_status = 2;  // 默认为失败状态

    // 初始化字符串字段为默认值
    strcpy(info->model, "Unknown");
    strcpy(info->vendor, "Unknown");
    strcpy(info->serial, "");
    strcpy(info->firmware_rev, "");
    strcpy(info->nominal_capacity_str, "");

    // 初始化数值字段
    info->capacity_gb = 0.0;
    info->total_sectors = 0;
    info->logical_block_size = 0;
    info->physical_block_size = 0;
    info->optimal_io_size = 0;

    printf("\033[94m【设备初始化】\033[m已初始化设备信息结构: %s\n", dev_path);
}

/**
 * 主要的设备信息收集函数
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 */
int collect_device_info(DeviceInfo* info) {
    if (!info) {
        fprintf(stderr, "错误: 设备信息结构指针为空\n");
        return -1;
    }

    int result = 0;
    int collection_attempts = 0;

    printf("\033[94m【信息收集】\033[m开始收集设备信息: %s\n", info->dev_path);

    // 首先收集通用信息（设备名、基本几何信息等）
    collection_attempts++;
    if (collect_generic_info(info) == 0) {
        printf("\033[94m【信息收集】\033[m通用信息收集成功\n");
        result = 0;
    } else {
        printf("\033[33m【信息收集】\033[m通用信息收集失败\n");
        result = -1;
    }

    // 从设备名进行初步类型判断
    determine_device_type_from_name(info);

    // 根据设备名和初步判断结果进行分发收集
    if (strncmp(info->main_dev_name, "nvme", 4) == 0) {
        // NVMe 设备
        printf("\033[94m【设备分发】\033[m检测到 NVMe 设备，使用 NVMe 收集器\n");
        info->bus_type = BUS_TYPE_NVME;
        info->device_type = DEVICE_TYPE_NVME_SSD;
        info->is_rotational = 0;

        collection_attempts++;
        if (collect_nvme_info(info) == 0) {
            printf("\033[94m【信息收集】\033[m NVMe 信息收集成功\n");
            result = 0;
        } else {
            printf("\033[33m【信息收集】\033[m NVMe 信息收集失败，使用通用信息\n");
        }
    }
    else if (strncmp(info->main_dev_name, "sd", 2) == 0) {
        // SATA/SCSI 设备需要进一步判断
        printf("\033[94m【设备分发】\033[m检测到 SCSI/SATA 设备，使用 SATA 收集器\n");

        collection_attempts++;
        if (collect_sata_info(info) == 0) {
            printf("\033[94m【信息收集】\033[m SATA/SCSI 信息收集成功\n");
            result = 0;
        } else {
            printf("\033[33m【信息收集】\033[m SATA/SCSI 信息收集失败，使用通用信息\n");
        }
    }
    else if (strncmp(info->main_dev_name, "hd", 2) == 0) {
        // PATA 设备
        printf("\033[94m【设备分发】\033[m检测到 PATA 设备，使用 SATA 收集器\n");
        info->bus_type = BUS_TYPE_PATA;

        collection_attempts++;
        if (collect_sata_info(info) == 0) {
            printf("\033[94m【信息收集】\033[m PATA 信息收集成功\n");
            result = 0;
        } else {
            printf("\033[33m【信息收集】\033[m PATA 信息收集失败，使用通用信息\n");
        }
    }
    else if (strncmp(info->main_dev_name, "mmcblk", 6) == 0) {
        // MMC/SD 卡
        printf("\033[94m【设备分发】\033[m检测到 MMC/SD 设备\n");
        info->bus_type = BUS_TYPE_MMC;
        info->device_type = DEVICE_TYPE_UNKNOWN_SSD;
        info->is_rotational = 0;
        // MMC 设备主要依赖通用信息收集
    }
    else if (strncmp(info->main_dev_name, "vd", 2) == 0) {
        // Virtio 设备
        printf("\033[94m【设备分发】\033[m检测到 Virtio 虚拟设备\n");
        info->bus_type = BUS_TYPE_VIRTIO;
        info->device_type = DEVICE_TYPE_UNKNOWN;
        // Virtio 设备主要依赖通用信息收集
    }
    else {
        // 其他类型设备或无法识别的设备
        printf("\033[94m【设备分发】\033[m未知设备类型，尝试进一步检测\n");

        // 检查是否为 USB 设备
        if (info->bus_type == BUS_TYPE_USB) {
            printf("\033[94m【设备分发】\033[m检测到 USB 设备，使用 USB 收集器\n");
            collection_attempts++;
            if (collect_usb_info(info) == 0) {
                printf("\033[94m【信息收集】\033[m USB 信息收集成功\n");
                result = 0;
            } else {
                printf("\033[33m【信息收集】\033[m USB 信息收集失败，使用通用信息\n");
            }
        }
    }

    // 应用设备特定的默认值
    apply_device_specific_defaults(info);

    // 最后的状态检查和清理
    finalize_device_info(info);

    printf("\033[94m【信息收集】\033[m设备信息收集完成，尝试了 %d 个收集器\n", collection_attempts);

    return result;
}

/**
 * 从设备名判断设备类型
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 */
static int determine_device_type_from_name(DeviceInfo* info) {
    if (!info || strlen(info->main_dev_name) == 0) {
        return -1;
    }

    const char* dev_name = info->main_dev_name;

    if (strncmp(dev_name, "nvme", 4) == 0) {
        info->bus_type = BUS_TYPE_NVME;
        info->device_type = DEVICE_TYPE_NVME_SSD;
        info->is_rotational = 0;
        printf("\033[94m【设备识别】\033[m从设备名识别为 NVMe SSD\n");
    }
    else if (strncmp(dev_name, "sd", 2) == 0) {
        // sd 设备可能是 SATA, SCSI, 或 USB，需要进一步判断
        printf("\033[94m【设备识别】\033[m从设备名识别为 SCSI/SATA 设备，需进一步判断\n");
        // 暂时不设置具体类型，等待进一步收集信息
    }
    else if (strncmp(dev_name, "hd", 2) == 0) {
        info->bus_type = BUS_TYPE_PATA;
        printf("\033[94m【设备识别】\033[m从设备名识别为 PATA 设备\n");
    }
    else if (strncmp(dev_name, "mmcblk", 6) == 0) {
        info->bus_type = BUS_TYPE_MMC;
        info->device_type = DEVICE_TYPE_UNKNOWN_SSD;
        info->is_rotational = 0;
        printf("\033[94m【设备识别】\033[m从设备名识别为 MMC/SD 设备\n");
    }
    else if (strncmp(dev_name, "vd", 2) == 0) {
        info->bus_type = BUS_TYPE_VIRTIO;
        printf("\033[94m【设备识别】\033[m从设备名识别为 Virtio 虚拟设备\n");
    }
    else {
        printf("\033[33m【设备识别】\033[m无法从设备名 '%s' 识别设备类型\n", dev_name);
    }

    return 0;
}

/**
 * 应用设备特定的默认值
 *
 * @param info 设备信息结构指针
 */
static void apply_device_specific_defaults(DeviceInfo* info) {
    if (!info) return;

    // 确保块大小有合理的默认值
    if (info->logical_block_size == 0) {
        info->logical_block_size = 512;  // 最常见的默认值
    }

    if (info->physical_block_size == 0) {
        if (info->device_type == DEVICE_TYPE_HDD) {
            // 现代大容量硬盘通常是 4K 物理扇区
            info->physical_block_size = (info->capacity_gb > 500) ? 4096 : 512;
        } else {
            // SSD 和其他设备
            info->physical_block_size = info->logical_block_size;
        }
    }

    // 设置合理的最优 I/O 大小
    if (info->optimal_io_size == 0) {
        switch (info->device_type) {
            case DEVICE_TYPE_NVME_SSD:
                info->optimal_io_size = 4096;  // NVMe 通常 4K 对齐
                break;
            case DEVICE_TYPE_SATA_SSD:
            case DEVICE_TYPE_UNKNOWN_SSD:
                info->optimal_io_size = 4096;  // SSD 通常 4K 对齐
                break;
            case DEVICE_TYPE_HDD:
                info->optimal_io_size = info->physical_block_size;
                break;
            case DEVICE_TYPE_USB_STORAGE:
                info->optimal_io_size = 4096;  // USB 设备通常 4K 对齐
                break;
            default:
                info->optimal_io_size = 4096;  // 安全的默认值
                break;
        }
    }

    printf("\033[94m【默认值应用】\033[m应用设备特定默认值完成\n");
}

/**
 * 最终化设备信息
 *
 * @param info 设备信息结构指针
 */
static void finalize_device_info(DeviceInfo* info) {
    if (!info) return;

    // 确保设备类型和总线类型的一致性
    if (info->device_type == DEVICE_TYPE_UNKNOWN) {
        if (info->bus_type == BUS_TYPE_NVME) {
            info->device_type = DEVICE_TYPE_NVME_SSD;
            info->is_rotational = 0;
        } else if (info->is_rotational == 0) {
            if (info->bus_type == BUS_TYPE_SATA || info->bus_type == BUS_TYPE_ATA) {
                info->device_type = DEVICE_TYPE_SATA_SSD;
            } else if (info->bus_type == BUS_TYPE_USB) {
                info->device_type = DEVICE_TYPE_USB_STORAGE;
            } else {
                info->device_type = DEVICE_TYPE_UNKNOWN_SSD;
            }
        } else if (info->is_rotational == 1) {
            info->device_type = DEVICE_TYPE_HDD;
        }
    }

    // 确保 rotational 状态与设备类型一致
    if (info->device_type == DEVICE_TYPE_NVME_SSD ||
        info->device_type == DEVICE_TYPE_SATA_SSD ||
        info->device_type == DEVICE_TYPE_UNKNOWN_SSD ||
        info->device_type == DEVICE_TYPE_USB_STORAGE) {
        info->is_rotational = 0;
        info->rpm = 0;
        info->rotation_rate_rpm = 0;  // 同步两个字段
    } else if (info->device_type == DEVICE_TYPE_HDD) {
        info->is_rotational = 1;
        if (info->rotation_rate_rpm == 0 && info->rpm == 0) {
            info->rotation_rate_rpm = 7200;  // 默认转速
            info->rpm = 7200;  // 同步
        } else if (info->rotation_rate_rpm == 0) {
            info->rotation_rate_rpm = info->rpm;  // 从 rpm 同步
        } else if (info->rpm == 0) {
            info->rpm = info->rotation_rate_rpm;  // 从 rotation_rate_rpm 同步
        }
    }

    // 重新计算容量（如果需要）
    if (info->capacity_gb == 0 && info->total_sectors > 0 && info->logical_block_size > 0) {
        double total_bytes = (double)info->total_sectors * 512;  // total_sectors 始终以 512 字节计
        info->capacity_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
    }

    // 设置信息收集状态
    int info_fields = 0;
    int filled_fields = 0;

    // 检查关键信息字段
    info_fields += 6; // model, vendor, capacity, sectors, block_size, device_type
    if (strlen(info->model) > 0 && strcmp(info->model, "Unknown") != 0) filled_fields++;
    if (strlen(info->vendor) > 0 && strcmp(info->vendor, "Unknown") != 0) filled_fields++;
    if (info->capacity_gb > 0) filled_fields++;
    if (info->total_sectors > 0) filled_fields++;
    if (info->logical_block_size > 0) filled_fields++;
    if (info->device_type != DEVICE_TYPE_UNKNOWN) filled_fields++;

    if (filled_fields >= info_fields * 0.8) {
        info->info_collection_status = 0; // 成功
    } else if (filled_fields >= info_fields * 0.5) {
        info->info_collection_status = 1; // 部分成功
    } else {
        info->info_collection_status = 2; // 失败
    }

    printf("\033[94m【信息最终化】\033[m设备信息最终化完成，状态: %d，完整度: %d/%d\n",
           info->info_collection_status, filled_fields, info_fields);
}

/**
 * 类型转换函数 - 获取设备类型字符串
 * 接受 DeviceInfo 指针，可以包含转速等详细信息
 */
const char* get_device_type_str(const DeviceInfo* info) {
    if (!info) return "未知设备";

    static char type_buffer[256];  // 静态缓冲区用于返回复杂字符串

    switch (info->device_type) {
        case DEVICE_TYPE_HDD:
            if (info->rotation_rate_rpm > 0) {
                snprintf(type_buffer, sizeof(type_buffer), "机械硬盘 (%d RPM)", info->rotation_rate_rpm);
            } else {
                strcpy(type_buffer, "机械硬盘");
            }
            return type_buffer;

        case DEVICE_TYPE_SATA_SSD:
            return "SATA 固态硬盘";

        case DEVICE_TYPE_NVME_SSD:
            return "NVMe 固态硬盘";

        case DEVICE_TYPE_USB_STORAGE:
            return "USB 存储设备";

        case DEVICE_TYPE_UNKNOWN_SSD:
            return "未知类型固态硬盘";

        case DEVICE_TYPE_UNKNOWN:
        default:
            return "未知设备";
    }
}

/**
 * 类型转换函数 - 获取总线类型字符串
 */
const char* get_bus_type_str(BusType type) {
    switch (type) {
        case BUS_TYPE_SATA: return "SATA";
        case BUS_TYPE_PATA: return "PATA";
        case BUS_TYPE_SCSI: return "SCSI/SAS";
        case BUS_TYPE_USB: return "USB";
        case BUS_TYPE_NVME: return "NVMe";
        case BUS_TYPE_MMC: return "MMC";
        case BUS_TYPE_VIRTIO: return "Virtio";
        case BUS_TYPE_ATA: return "ATA";
        case BUS_TYPE_UNKNOWN:
        default: return "未知";
    }
}

/**
 * 获取兼容的设备类型字符串（简化版）
 */
const char* get_device_type_legacy_str(const DeviceInfo* info) {
    if (!info) return "Unknown";

    if (info->device_type == DEVICE_TYPE_NVME_SSD) return "NVMe";
    if (info->device_type == DEVICE_TYPE_SATA_SSD ||
        info->device_type == DEVICE_TYPE_UNKNOWN_SSD) return "SSD";
    if (info->device_type == DEVICE_TYPE_HDD) return "HDD";
    if (info->device_type == DEVICE_TYPE_USB_STORAGE) return "USB";
    return "Unknown";
}

/**
 * 设备特性判断函数 - 是否为 SSD 设备
 */
int is_ssd_device(const DeviceInfo* info) {
    if (!info) return 0;

    return (info->device_type == DEVICE_TYPE_SATA_SSD ||
            info->device_type == DEVICE_TYPE_NVME_SSD ||
            info->device_type == DEVICE_TYPE_UNKNOWN_SSD ||
            (info->device_type == DEVICE_TYPE_USB_STORAGE && info->is_rotational == 0) ||
            info->is_rotational == 0);
}

/**
 * 设备特性判断函数 - 是否为 HDD 设备
 */
int is_hdd_device(const DeviceInfo* info) {
    if (!info) return 0;

    return (info->device_type == DEVICE_TYPE_HDD ||
            info->is_rotational == 1);
}

/**
 * 设备特性判断函数 - 是否为 NVMe 设备
 */
int is_nvme_device(const DeviceInfo* info) {
    if (!info) return 0;

    return (info->device_type == DEVICE_TYPE_NVME_SSD ||
            info->bus_type == BUS_TYPE_NVME);
}

/**
 * 根据设备类型推荐可疑块阈值
 */
int get_recommended_suspect_threshold(const DeviceInfo* info) {
    if (!info) return 100;  // 默认值

    if (is_ssd_device(info)) {
        if (is_nvme_device(info)) {
            return 10;  // NVMe SSD 更快，阈值更低
        } else {
            return 20;  // SATA SSD
        }
    } else if (is_hdd_device(info)) {
        if (info->rotation_rate_rpm >= 10000) {
            return 60;  // 高速硬盘 (10K RPM)
        } else if (info->rotation_rate_rpm >= 7200 || info->rotation_rate_rpm == 0) {
            return 100; // 7200 RPM 或未知转速
        } else {
            return 150; // 5400 RPM 或更慢
        }
    } else if (info->device_type == DEVICE_TYPE_USB_STORAGE) {
        return 200;  // USB 设备通常较慢
    }

    return 100; // 默认值
}

/**
 * 打印设备信息
 */
void print_device_info(const DeviceInfo* info) {
    if (!info) {
        printf("\033[31m【设备信息】\033[m设备信息结构为空\n");
        return;
    }

    printf("\033[1;94m【设备信息】\033[m设备路径: %s\n", info->dev_path);
    printf("\033[1;94m【设备信息】\033[m主设备名: %s\n", info->main_dev_name);
    printf("\033[1;94m【设备信息】\033[m设备类型: %s\n", get_device_type_str(info));
    printf("\033[1;94m【设备信息】\033[m接口类型: %s\n", get_bus_type_str(info->bus_type));

    if (strlen(info->vendor) > 0 && strcmp(info->vendor, "Unknown") != 0) {
        printf("\033[1;94m【设备信息】\033[m厂商: %s\n", info->vendor);
    }
    if (strlen(info->model) > 0 && strcmp(info->model, "Unknown") != 0) {
        printf("\033[1;94m【设备信息】\033[m型号: %s\n", info->model);
    }
    if (strlen(info->serial) > 0) {
        printf("\033[1;94m【设备信息】\033[m序列号: %s\n", info->serial);
    }
    if (strlen(info->firmware_rev) > 0) {
        printf("\033[1;94m【设备信息】\033[m固件版本: %s\n", info->firmware_rev);
    }

    if (info->capacity_gb > 0) {
        printf("\033[1;94m【设备信息】\033[m容量: %.2f GB", info->capacity_gb);
        if (strlen(info->nominal_capacity_str) > 0) {
            printf(" (标称 %s)", info->nominal_capacity_str);
        }
        printf("\n");
    }

    if (info->total_sectors > 0) {
        printf("\033[1;94m【设备信息】\033[m总扇区数: %llu\n", info->total_sectors);
    }

    if (info->logical_block_size > 0) {
        printf("\033[1;94m【设备信息】\033[m逻辑块大小: %u 字节\n", info->logical_block_size);
    }

    if (info->physical_block_size > 0 && info->physical_block_size != info->logical_block_size) {
        printf("\033[1;94m【设备信息】\033[m物理块大小: %u 字节\n", info->physical_block_size);
    }

    if (info->optimal_io_size > 0 && info->optimal_io_size != info->logical_block_size) {
        printf("\033[1;94m【设备信息】\033[m最优I/O大小: %u 字节\n", info->optimal_io_size);
    }

    if (is_hdd_device(info)) {
        printf("\033[1;94m【设备信息】\033[m机械硬盘: 是\n");
        if (info->rotation_rate_rpm > 0) {
            printf("\033[1;94m【设备信息】\033[m转速: %d RPM\n", info->rotation_rate_rpm);
        }
    } else if (is_ssd_device(info)) {
        printf("\033[1;94m【设备信息】\033[m固态硬盘: 是\n");
    }

    // 显示信息收集状态
    const char* status_str;
    const char* status_color;
    switch (info->info_collection_status) {
        case 0: status_str = "完整"; status_color = "\033[32m"; break;
        case 1: status_str = "部分"; status_color = "\033[33m"; break;
        default: status_str = "基本"; status_color = "\033[31m"; break;
    }
    printf("\033[1;94m【设备信息】\033[m信息收集状态: %s%s\033[0m\n", status_color, status_str);

    // 推荐的扫描参数
    int recommended_threshold = get_recommended_suspect_threshold(info);
    printf("\033[1;94m【设备信息】\033[m推荐可疑块阈值: %d 毫秒\n", recommended_threshold);

    printf("\n");
}
