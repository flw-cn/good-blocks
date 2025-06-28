// src/device_info/nvme_info.c - NVMe 设备信息收集模块

#include "nvme_info.h"
#include "generic_info.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <math.h>

// 条件编译：根据编译选项选择不同实现
#if USE_SYSTEM_COMMANDS

// 基于系统命令的实现函数声明
#if USE_NVME_CLI
static int collect_nvme_cli_info_cmd(DeviceInfo* info);
static int collect_nvme_namespace_info(DeviceInfo* info);
static int collect_nvme_controller_info(DeviceInfo* info);
static const char* map_pci_vendor_id(unsigned int vendor_id);  // 添加这个声明
#endif

#if USE_SMARTCTL
static int collect_smartctl_nvme_info_cmd(DeviceInfo* info);
#endif

static int collect_nvme_info_cmd(DeviceInfo* info);

#else
// 基于 C API 的实现函数声明
static int collect_nvme_info_api(DeviceInfo* info);
#endif

// 通用辅助函数声明
static int get_nvme_optimal_io_size(DeviceInfo* info);
static void finalize_nvme_info(DeviceInfo* info);

/**
 * NVMe 设备信息收集主函数
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 *
 * 根据编译选项选择使用系统命令或 C API 进行信息收集
 */
int collect_nvme_info(DeviceInfo* info) {
    if (!info) return -1;

    // 确保设备类型设置正确
    info->bus_type = BUS_TYPE_NVME;
    info->device_type = DEVICE_TYPE_NVME_SSD;
    info->is_rotational = 0;

    int result;

#if USE_SYSTEM_COMMANDS
    result = collect_nvme_info_cmd(info);
#else
    result = collect_nvme_info_api(info);
#endif

    // 最终化 NVMe 设备信息
    finalize_nvme_info(info);

    return result;
}

#if USE_SYSTEM_COMMANDS
/**
 * 使用系统命令的 NVMe 信息收集实现
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 *
 * 优先使用 nvme-cli，然后尝试 smartctl，最后使用通用方法
 */
static int collect_nvme_info_cmd(DeviceInfo* info) {
    int success_count = 0;
    int total_attempts = 0;

    printf("\033[36m【NVMe 信息】\033[m开始收集 NVMe 设备信息...\n");

    // 首先收集通用信息（sysfs, udevadm）
    total_attempts++;
    if (collect_generic_info(info) == 0) {
        success_count++;
        printf("\033[36m【NVMe 信息】\033[m通用信息收集成功\n");
    } else {
        printf("\033[33m【NVMe 警告】\033[m通用信息收集失败\n");
    }

#if USE_NVME_CLI
    // 尝试使用 nvme-cli 获取详细信息
    total_attempts++;
    printf("\033[36m【NVMe 信息】\033[m尝试使用 nvme-cli 收集信息...\n");
    if (collect_nvme_cli_info_cmd(info) == 0) {
        success_count++;
        printf("\033[36m【NVMe 信息】\033[m nvme-cli 信息收集成功\n");
    } else {
        printf("\033[33m【NVMe 警告】\033[m nvme-cli 信息收集失败，可能需要安装 nvme-cli\n");
    }
#endif

#if USE_SMARTCTL
    // 尝试使用 smartctl 获取 SMART 信息
    total_attempts++;
    printf("\033[36m【NVMe 信息】\033[m尝试使用 smartctl 收集信息...\n");
    if (collect_smartctl_nvme_info_cmd(info) == 0) {
        success_count++;
        printf("\033[36m【NVMe 信息】\033[m smartctl 信息收集成功\n");
    } else {
        printf("\033[33m【NVMe 警告】\033[m smartctl 信息收集失败\n");
    }
#endif

    // 获取 NVMe 特定的最优 I/O 大小
    get_nvme_optimal_io_size(info);

    printf("\033[36m【NVMe 信息】\033[m信息收集完成，成功 %d/%d 项\n", success_count, total_attempts);

    return (success_count > 0) ? 0 : -1;
}

#if USE_NVME_CLI
/**
 * 使用 nvme-cli 命令收集 NVMe 信息
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 */
static int collect_nvme_cli_info_cmd(DeviceInfo* info) {
    int found_info = 0;

    // 检查 nvme-cli 是否可用
    if (system("which nvme >/dev/null 2>&1") != 0) {
        printf("\033[33m【NVMe 警告】\033[m nvme-cli 工具未找到\n");
        return -1;
    }

    // 获取 Namespace 信息
    found_info += collect_nvme_namespace_info(info);

    // 获取 Controller 信息
    found_info += collect_nvme_controller_info(info);

    return found_info > 0 ? 0 : -1;
}

/**
 * 收集 NVMe Namespace 信息
 *
 * @param info 设备信息结构指针
 * @return 收集到的信息项数
 */
static int collect_nvme_namespace_info(DeviceInfo* info) {
    char command[512];
    snprintf(command, sizeof(command), "nvme id-ns %s 2>/dev/null", info->dev_path);

    char* output = run_command_output(command);
    if (!output) {
        return 0;
    }

    char buffer[256];
    int found_info = 0;

    // 提取 LBA 大小 - nvme-cli 输出格式: "lbaf  0 : ms:0   lbads:9  rp:0 (in use)"
    // 我们需要找到包含 "in use" 的行，然后提取 lbads 值
    if (find_line_and_extract_value(output, buffer, sizeof(buffer), PARSER_AUTO,
                                   "in use", NULL)) {
        // 在这行中查找 lbads 值
        char* lbads_str = strstr(buffer, "lbads:");
        if (lbads_str) {
            int lbads = strtol(lbads_str + 6, NULL, 10);  // lbads: 后面的数字
            if (lbads >= 0 && lbads <= 16) {  // 合理范围检查
                int lba_size = 1 << lbads;  // LBA 大小 = 2^lbads
                info->logical_block_size = lba_size;
                info->physical_block_size = lba_size;
                found_info++;
                printf("\033[36m【NVMe 详细】\033[m LBA 大小: %d 字节 (lbads=%d)\n", lba_size, lbads);
            }
        }
    }

    // 提取 Namespace Size (nsze)
    if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_COLON,
                                 "nsze", "Namespace Size", NULL)) {
        unsigned long long nsze = strtoull(buffer, NULL, 0);
        if (nsze > 0 && info->logical_block_size > 0) {
            // nsze 是以 LBA 为单位的，转换为 512 字节扇区
            info->total_sectors = nsze * info->logical_block_size / 512;

            // 计算容量
            double total_bytes = (double)nsze * info->logical_block_size;
            info->capacity_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
            found_info++;

            printf("\033[36m【NVMe 详细】\033[m Namespace 大小: %llu LBA\n", nsze);
            printf("\033[36m【NVMe 详细】\033[m 计算容量: %.2f GB\n", info->capacity_gb);
        }
    }

    // 提取 Namespace Capacity (ncap)
    if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_COLON,
                                 "ncap", "Namespace Capacity", NULL)) {
        unsigned long long ncap = strtoull(buffer, NULL, 0);
        if (ncap > 0) {
            printf("\033[36m【NVMe 详细】\033[m Namespace 容量: %llu LBA\n", ncap);
            found_info++;
        }
    }

    // 提取 Namespace Utilization (nuse)
    if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_COLON,
                                 "nuse", "Namespace Utilization", NULL)) {
        unsigned long long nuse = strtoull(buffer, NULL, 0);
        if (nuse > 0) {
            printf("\033[36m【NVMe 详细】\033[m Namespace 使用: %llu LBA\n", nuse);
            found_info++;
        }
    }

    free(output);
    return found_info;
}

/**
 * 收集 NVMe Controller 信息
 *
 * @param info 设备信息结构指针
 * @return 收集到的信息项数
 */
static int collect_nvme_controller_info(DeviceInfo* info) {
    char command[512];
    snprintf(command, sizeof(command), "nvme id-ctrl %s 2>/dev/null", info->dev_path);

    char* output = run_command_output(command);
    if (!output) {
        return 0;
    }

    char buffer[256];
    int found_info = 0;

    // 提取型号 (Model Number - mn)
    if (strlen(info->model) == 0 || strcmp(info->model, "Unknown") == 0) {
        if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_COLON,
                                     "mn", "Model Number", "model", NULL)) {
            // 去除前后空格
            char* trimmed = trim_whitespace(buffer);
            if (strlen(trimmed) > 0 && strlen(trimmed) < sizeof(info->model)) {
                strcpy(info->model, trimmed);
                found_info++;
                printf("\033[36m【NVMe 详细】\033[m 型号: %s\n", trimmed);
            }
        }
    }

    // 提取序列号 (Serial Number - sn)
    if (strlen(info->serial) == 0) {
        if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_COLON,
                                     "sn", "Serial Number", "serial", NULL)) {
            char serial_buffer[MAX_SERIAL_LEN];
            if (extract_first_word(buffer, serial_buffer, sizeof(serial_buffer))) {
                strcpy(info->serial, serial_buffer);
                found_info++;
                printf("\033[36m【NVMe 详细】\033[m 序列号: %s\n", serial_buffer);
            }
        }
    }

    // 提取固件版本 (Firmware Revision - fr)
    if (strlen(info->firmware_rev) == 0) {
        if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_COLON,
                                     "fr", "Firmware Revision", "firmware", NULL)) {
            char fw_buffer[MAX_FW_REV_LEN];
            if (extract_first_word(buffer, fw_buffer, sizeof(fw_buffer))) {
                strcpy(info->firmware_rev, fw_buffer);
                found_info++;
                printf("\033[36m【NVMe 详细】\033[m 固件版本: %s\n", fw_buffer);
            }
        }
    }

    // 提取厂商 ID (Vendor ID - vid)
    if (strlen(info->vendor) == 0 || strcmp(info->vendor, "Unknown") == 0) {
        if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_COLON,
                                     "vid", "Vendor ID", NULL)) {
            unsigned int vid = strtoul(buffer, NULL, 0);
            if (vid > 0) {
                const char* vendor_name = map_pci_vendor_id(vid);
                if (vendor_name) {
                    strncpy(info->vendor, vendor_name, sizeof(info->vendor) - 1);
                    info->vendor[sizeof(info->vendor) - 1] = '\0';
                    found_info++;
                    printf("\033[36m【NVMe 详细】\033[m 厂商: %s (ID: 0x%04x)\n", vendor_name, vid);
                } else {
                    // 如果没有映射到厂商名，至少记录一下ID
                    snprintf(info->vendor, sizeof(info->vendor), "VID_0x%04X", vid);
                    found_info++;
                    printf("\033[36m【NVMe 详细】\033[m 厂商 ID: 0x%04x\n", vid);
                }
            }
        }
    }

    free(output);
    return found_info;
}

/**
 * 简化的 PCI 厂商 ID 到厂商名称映射
 *
 * @param vendor_id PCI 厂商 ID
 * @return 厂商名称字符串，如果未知则返回 NULL
 */
static const char* map_pci_vendor_id(unsigned int vendor_id) {
    switch (vendor_id) {
        case 0x8086: return "Intel";
        case 0x144d: return "Samsung";
        case 0x15b7: return "SanDisk";
        case 0x1179: return "Toshiba";
        case 0x1c5c: return "SK Hynix";
        case 0x1987: return "Phison";
        case 0x126f: return "Silicon Motion";
        case 0x1cc1: return "ADATA";
        case 0x1344: return "Micron";
        case 0xc0a9: return "Crucial";
        case 0x1e0f: return "KIOXIA";
        case 0x1bb1: return "Seagate";
        case 0x1c58: return "HGST";
        case 0x1b96: return "Western Digital";
        case 0x1f40: return "Netac";
        case 0x1d97: return "Shenzhen Longsys";
        case 0x1e49: return "Yangtze Memory";
        case 0x1e95: return "Solid State Storage";
        case 0x1f03: return "Corsair";
        case 0x1b4b: return "Marvell";
        case 0x14a4: return "Lite-On";
        case 0x1636: return "Elex";
        case 0x1e3d: return "Fungible";
        case 0x1dee: return "Biwin Storage";
        case 0x1dbe: return "KIOXIA America";
        case 0x1e4b: return "MAXIO";
        case 0x1e49: return "YMTC";
        case 0x126f: return "SMI";
        default: return NULL;
    }
}
#endif

#if USE_SMARTCTL
/**
 * 使用 smartctl 命令收集 NVMe SMART 信息
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 */
static int collect_smartctl_nvme_info_cmd(DeviceInfo* info) {
    char command[512];
    snprintf(command, sizeof(command), "sudo smartctl -a %s 2>/dev/null", info->dev_path);

    char* smartctl_output = run_command_output(command);
    if (!smartctl_output) {
        return -1;
    }

    char buffer[256];
    int found_info = 0;

    // 提取 NVMe 特定的 LBA 大小信息
    if (extract_value_from_output(smartctl_output, buffer, sizeof(buffer), PARSER_COLON,
                                 "LBA Size", "Sector Size", NULL)) {
        int lba_size = strtol(buffer, NULL, 10);
        if (lba_size > 0 && lba_size <= 65536) {  // 合理范围检查
            info->logical_block_size = lba_size;
            info->physical_block_size = lba_size;
            found_info++;
            printf("\033[36m【smartctl】\033[m LBA 大小: %d 字节\n", lba_size);
        }
    }

    // 提取型号信息
    if ((strlen(info->model) == 0 || strcmp(info->model, "Unknown") == 0) &&
        extract_value_from_output(smartctl_output, buffer, sizeof(buffer), PARSER_COLON,
                              "Device Model", "Model Number", "Product", NULL)) {
        char* trimmed = trim_whitespace(buffer);
        if (strlen(trimmed) > 0 && strlen(trimmed) < sizeof(info->model)) {
            strcpy(info->model, trimmed);
            found_info++;
            printf("\033[36m【smartctl】\033[m 型号: %s\n", trimmed);
        }
    }

    // 提取序列号
    if (strlen(info->serial) == 0 &&
        extract_value_from_output(smartctl_output, buffer, sizeof(buffer), PARSER_COLON,
                              "Serial Number", "Serial number", NULL)) {
        char serial_buffer[MAX_SERIAL_LEN];
        if (extract_first_word(buffer, serial_buffer, sizeof(serial_buffer))) {
            strcpy(info->serial, serial_buffer);
            found_info++;
            printf("\033[36m【smartctl】\033[m 序列号: %s\n", serial_buffer);
        }
    }

    // 提取固件版本
    if (strlen(info->firmware_rev) == 0 &&
        extract_value_from_output(smartctl_output, buffer, sizeof(buffer), PARSER_COLON,
                              "Firmware Version", "Revision", "FW Revision", NULL)) {
        char* trimmed = trim_whitespace(buffer);
        if (strlen(trimmed) > 0 && strlen(trimmed) < sizeof(info->firmware_rev)) {
            strcpy(info->firmware_rev, trimmed);
            found_info++;
            printf("\033[36m【smartctl】\033[m 固件版本: %s\n", trimmed);
        }
    }

    // 提取容量信息
    if (strlen(info->nominal_capacity_str) == 0 &&
        extract_bracketed_value(smartctl_output, buffer, sizeof(buffer),
                                       "Total NVM Capacity", "User Capacity", NULL)) {
        if (strlen(buffer) < sizeof(info->nominal_capacity_str)) {
            strcpy(info->nominal_capacity_str, buffer);
            found_info++;
            printf("\033[36m【smartctl】\033[m 标称容量: %s\n", buffer);
        }
    }

    free(smartctl_output);
    return found_info > 0 ? 0 : -1;
}
#endif

#else
/**
 * 使用 C API 的 NVMe 信息收集实现（占位符）
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 *
 * 注意：这里是未来基于 libnvme 等 C 库的实现预留位置
 * 目前先回退到通用信息收集方法
 */
static int collect_nvme_info_api(DeviceInfo* info) {
    printf("\033[33m【NVMe 警告】\033[m C API 版本尚未实现，使用通用方法收集信息\n");

    // TODO: 实现基于 libnvme 的信息收集
    // #include <libnvme.h>
    //
    // nvme_root_t root = nvme_scan();
    // nvme_host_t host = nvme_first_host(root);
    // nvme_subsystem_t subsys;
    // nvme_ctrl_t ctrl;
    // nvme_ns_t ns;
    //
    // nvme_for_each_subsystem(host, subsys) {
    //     nvme_subsystem_for_each_ctrl(subsys, ctrl) {
    //         if (strcmp(nvme_ctrl_get_name(ctrl), info->main_dev_name) == 0) {
    //             // 收集控制器信息
    //             strcpy(info->model, nvme_ctrl_get_model(ctrl));
    //             strcpy(info->serial, nvme_ctrl_get_serial(ctrl));
    //             strcpy(info->firmware_rev, nvme_ctrl_get_firmware(ctrl));
    //
    //             // 收集命名空间信息
    //             nvme_ctrl_for_each_ns(ctrl, ns) {
    //                 info->logical_block_size = nvme_ns_get_lba_size(ns);
    //                 info->total_sectors = nvme_ns_get_lba_count(ns);
    //                 break; // 只处理第一个命名空间
    //             }
    //             break;
    //         }
    //     }
    // }
    //
    // nvme_free_tree(root);

    // 目前回退到通用信息收集
    return collect_generic_info(info);
}
#endif

/**
 * 获取 NVMe 设备的最优 I/O 大小
 *
 * @param info 设备信息结构指针
 * @return 0 成功，-1 失败
 */
static int get_nvme_optimal_io_size(DeviceInfo* info) {
    // 如果没有设置最优 I/O 大小，为 NVMe 设备设置合理的默认值
    if (info->optimal_io_size == 0) {
        // NVMe 设备通常使用 4KB 或更大的页大小
        if (info->logical_block_size >= 4096) {
            info->optimal_io_size = info->logical_block_size;
        } else {
            // 大多数 NVMe SSD 的最优块大小是 4KB 或其倍数
            info->optimal_io_size = 4096;
        }

        printf("\033[36m【NVMe 优化】\033[m设置最优 I/O 大小: %u 字节\n", info->optimal_io_size);
    }

    return 0;
}

/**
 * 最终化 NVMe 设备信息
 *
 * @param info 设备信息结构指针
 *
 * 确保 NVMe 设备的所有字段都设置正确
 */
static void finalize_nvme_info(DeviceInfo* info) {
    // 确保 NVMe 设备的基本属性正确
    info->bus_type = BUS_TYPE_NVME;
    info->device_type = DEVICE_TYPE_NVME_SSD;
    info->is_rotational = 0;
    info->rotation_rate_rpm = 0;  // NVMe SSD 没有转速

    // 如果没有设置逻辑块大小，使用默认值
    if (info->logical_block_size == 0) {
        info->logical_block_size = 512;  // NVMe 默认
        info->physical_block_size = 512;
    }

    // 确保物理块大小与逻辑块大小一致（对于 NVMe）
    if (info->physical_block_size == 0) {
        info->physical_block_size = info->logical_block_size;
    }

    // 确保最优 I/O 大小设置
    get_nvme_optimal_io_size(info);

    // 如果从 sysfs 获取的容量为 0，但有 total_sectors，重新计算
    if (info->capacity_gb == 0 && info->total_sectors > 0) {
        double total_bytes = (double)info->total_sectors * 512;  // total_sectors 总是以 512 字节为单位
        info->capacity_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
    }

    // 验证数据的合理性
    if (info->logical_block_size > 0 && info->total_sectors > 0) {
        double calculated_gb = (double)info->total_sectors * 512 / (1024.0 * 1024.0 * 1024.0);
        if (info->capacity_gb == 0 || fabs(info->capacity_gb - calculated_gb) > calculated_gb * 0.1) {
            // 如果容量差异超过10%，使用计算值
            info->capacity_gb = calculated_gb;
        }
    }

    printf("\033[36m【NVMe 最终】\033[m设备信息最终化完成\n");
    printf("\033[36m【NVMe 最终】\033[m - 设备类型: %s\n", get_device_type_str(info));
    printf("\033[36m【NVMe 最终】\033[m - 接口类型: %s\n", get_bus_type_str(info->bus_type));
    printf("\033[36m【NVMe 最终】\033[m - 逻辑块大小: %u 字节\n", info->logical_block_size);
    printf("\033[36m【NVMe 最终】\033[m - 最优 I/O 大小: %u 字节\n", info->optimal_io_size);
    if (info->capacity_gb > 0) {
        printf("\033[36m【NVMe 最终】\033[m - 容量: %.2f GB\n", info->capacity_gb);
    }
}
