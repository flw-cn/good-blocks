// src/device_info/nvme_cmd.c - 基于 nvme-cli 命令行的实现
#define _GNU_SOURCE

#include "nvme_cmd.h"

#if USE_SYSTEM_COMMANDS && USE_NVME_CLI

#include "generic_info.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// 基于 nvme-cli 命令的信息收集
int collect_nvme_cli_info_cmd(DeviceInfo* info) {
    int found_info = 0;

    // 获取 Namespace 信息
    found_info += collect_nvme_namespace_info(info);

    // 获取 Controller 信息
    found_info += collect_nvme_controller_info(info);

    return found_info > 0 ? 0 : -1;
}

// 收集 NVMe Namespace 信息
static int collect_nvme_namespace_info(DeviceInfo* info) {
    char command[256];
    snprintf(command, sizeof(command), "nvme id-ns %s 2>/dev/null", info->dev_path);

    char* output = run_command_output(command);
    if (!output) return 0;

    char* line = strtok(output, "\n");
    int found_info = 0;

    while (line != NULL) {
        // 解析 LBA 格式信息
        if (strstr(line, "LBA Format") && strstr(line, "in use")) {
            char* data_size_str = strstr(line, "Data Size:");
            if (data_size_str) {
                data_size_str += strlen("Data Size:");
                while (*data_size_str && isspace(*data_size_str)) data_size_str++;

                int lba_size = strtol(data_size_str, NULL, 10);
                if (lba_size > 0) {
                    info->logical_block_size = lba_size;
                    info->physical_block_size = lba_size;
                    found_info++;
                }
            }
        }
        // 解析 Namespace Size
        else if (strstr(line, "nsze")) {
            char* colon = strchr(line, ':');
            if (colon) {
                unsigned long long nsze = strtoull(colon + 1, NULL, 0);
                if (nsze > 0 && info->logical_block_size > 0) {
                    // nsze 是以 LBA 为单位的，转换为 512 字节扇区
                    info->total_sectors = nsze * info->logical_block_size / 512;

                    // 计算容量
                    double total_bytes = (double)nsze * info->logical_block_size;
                    info->capacity_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
                    found_info++;
                }
            }
        }
        // 解析 Namespace Capacity
        else if (strstr(line, "ncap")) {
            char* colon = strchr(line, ':');
            if (colon) {
                unsigned long long ncap = strtoull(colon + 1, NULL, 0);
                if (ncap > 0 && info->logical_block_size > 0) {
                    // 如果 ncap 与 nsze 不同，可能表示过度配置
                    found_info++;
                }
            }
        }

        line = strtok(NULL, "\n");
    }

    free(output);
    return found_info;
}

// 收集 NVMe Controller 信息
static int collect_nvme_controller_info(DeviceInfo* info) {
    char command[256];
    snprintf(command, sizeof(command), "nvme id-ctrl %s 2>/dev/null", info->dev_path);

    char* output = run_command_output(command);
    if (!output) return 0;

    char* line = strtok(output, "\n");
    int found_info = 0;

    while (line != NULL) {
        // 解析型号 (Model Number)
        if (strstr(line, "mn ") && (strlen(info->model) == 0 || strcmp(info->model, "Unknown") == 0)) {
            char* colon = strchr(line, ':');
            if (colon) {
                char* start = colon + 1;
                while (*start && isspace(*start)) start++;

                if (strlen(start) > 0) {
                    char* end = start + strlen(start) - 1;
                    while (end > start && isspace(*end)) *end-- = '\0';

                    if (strlen(start) < sizeof(info->model)) {
                        strcpy(info->model, start);
                        found_info++;
                    }
                }
            }
        }
        // 解析序列号 (Serial Number)
        else if (strstr(line, "sn ") && strlen(info->serial) == 0) {
            char* colon = strchr(line, ':');
            if (colon) {
                char* start = colon + 1;
                while (*start && isspace(*start)) start++;

                char* end = start;
                while (*end && !isspace(*end)) end++;

                size_t len = end - start;
                if (len > 0 && len < sizeof(info->serial)) {
                    strncpy(info->serial, start, len);
                    info->serial[len] = '\0';
                    found_info++;
                }
            }
        }
        // 解析固件版本 (Firmware Revision)
        else if (strstr(line, "fr ") && strlen(info->firmware_rev) == 0) {
            char* colon = strchr(line, ':');
            if (colon) {
                char* start = colon + 1;
                while (*start && isspace(*start)) start++;

                char* end = start;
                while (*end && !isspace(*end)) end++;

                size_t len = end - start;
                if (len > 0 && len < sizeof(info->firmware_rev)) {
                    strncpy(info->firmware_rev, start, len);
                    info->firmware_rev[len] = '\0';
                    found_info++;
                }
            }
        }
        // 解析厂商 ID (Vendor ID)
        else if (strstr(line, "vid ") && (strlen(info->vendor) == 0 || strcmp(info->vendor, "Unknown") == 0)) {
            // 暂时跳过，因为这只是一个数字 ID，需要映射到厂商名称
            // 可以在后续版本中添加 PCI ID 到厂商名称的映射
        }

        line = strtok(NULL, "\n");
    }

    free(output);
    return found_info;
}

#endif // USE_SYSTEM_COMMANDS && USE_NVME_CLI
