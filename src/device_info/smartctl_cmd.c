// src/device_info/smartctl_cmd.c - 基于 smartctl 命令行的实现
#define _GNU_SOURCE

#include "smartctl_cmd.h"

#if USE_SYSTEM_COMMANDS && USE_SMARTCTL

#include "generic_info.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// 基于 smartctl 命令的信息收集
int collect_smartctl_info_cmd(DeviceInfo* info) {
    char command[MAX_FULL_PATH_LEN + 32];
    snprintf(command, sizeof(command), "sudo smartctl -a %s 2>/dev/null", info->dev_path);

    char* smartctl_output = run_command_output(command);
    if (!smartctl_output) {
        return -1;
    }

    populate_device_info_from_smartctl_output(info, smartctl_output);

    free(smartctl_output);
    return 0;
}

// 从 smartctl 输出解析设备信息
void populate_device_info_from_smartctl_output(DeviceInfo* info, const char* smartctl_output) {
    if (!smartctl_output) return;

    char buffer[256];
    int found_info = 0;

    // 提取转速信息（仅用于HDD）
    if ((info->device_type == DEVICE_TYPE_HDD || info->is_rotational == 1) && info->rotation_rate_rpm == 0) {
        if (get_string_from_output(smartctl_output, buffer, sizeof(buffer), "Rotation Rate", NULL)) {
            if (isdigit(buffer[0])) {
                info->rotation_rate_rpm = strtol(buffer, NULL, 10);
                found_info = 1;
            }
        }
    }

    // 提取序列号
    if (strlen(info->serial) == 0) {
        if (get_string_from_output(smartctl_output, buffer, sizeof(buffer),
                                  "Serial Number", "Serial number", NULL)) {
            char serial_buffer[MAX_SERIAL_LEN];
            if (get_first_word(buffer, serial_buffer, sizeof(serial_buffer))) {
                strcpy(info->serial, serial_buffer);
                found_info = 1;
            }
        }
    }

    // 提取型号信息
    if (strlen(info->model) == 0 || strcmp(info->model, "Unknown") == 0) {
        if (get_string_from_output(smartctl_output, buffer, sizeof(buffer),
                                  "Device Model", "Model Number", "Product", NULL)) {
            if (strlen(buffer) < sizeof(info->model)) {
                strcpy(info->model, buffer);
                found_info = 1;
            }
        }
    }

    // 提取厂商信息
    if (strlen(info->vendor) == 0 || strcmp(info->vendor, "Unknown") == 0) {
        if (get_string_from_output(smartctl_output, buffer, sizeof(buffer),
                                  "Model Family", "Vendor", NULL)) {
            char vendor_buffer[MAX_VENDOR_LEN];
            if (get_first_word(buffer, vendor_buffer, sizeof(vendor_buffer))) {
                strcpy(info->vendor, vendor_buffer);
                found_info = 1;
            }
        }
    }

    // 提取固件版本信息
    if (strlen(info->firmware_rev) == 0) {
        if (get_string_from_output(smartctl_output, buffer, sizeof(buffer),
                                  "Firmware Version", "Revision", "FW Revision", NULL)) {
            if (strlen(buffer) < sizeof(info->firmware_rev)) {
                strcpy(info->firmware_rev, buffer);
                found_info = 1;
            }
        }
    }

    // 提取容量信息
    if (strlen(info->nominal_capacity_str) == 0) {
        if (get_bracketed_string_from_output(smartctl_output, buffer, sizeof(buffer),
                                           "User Capacity", "Total NVM Capacity", NULL)) {
            if (strlen(buffer) < sizeof(info->nominal_capacity_str)) {
                strcpy(info->nominal_capacity_str, buffer);
                found_info = 1;
            }
        }
    }

    // 对于 NVMe 设备，尝试获取特定信息
    if (info->device_type == DEVICE_TYPE_NVME_SSD || info->bus_type == BUS_TYPE_NVME) {
        // 提取 LBA 大小信息
        if (get_string_from_output(smartctl_output, buffer, sizeof(buffer),
                                  "LBA Size", "Sector Size", NULL)) {
            int lba_size = strtol(buffer, NULL, 10);
            if (lba_size > 0) {
                info->logical_block_size = lba_size;
                info->physical_block_size = lba_size;
                found_info = 1;
            }
        }
    }
}

#endif // USE_SYSTEM_COMMANDS && USE_SMARTCTL
