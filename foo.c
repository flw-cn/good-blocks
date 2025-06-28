// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h> // For PATH_MAX

#include "device_info.h"
#include "disk_collector.h" // Assuming this contains get_device_info

// Helper function to print device type
const char* get_device_type_str(DeviceType type) {
    switch (type) {
        case DEVICE_TYPE_HDD: return "HDD";
        case DEVICE_TYPE_SATA_SSD: return "SATA SSD";
        case DEVICE_TYPE_NVME_SSD: return "NVMe SSD";
        case DEVICE_TYPE_USB_STORAGE: return "USB Storage";
        case DEVICE_TYPE_UNKNOWN_SSD: return "Unknown SSD";
        case DEVICE_TYPE_UNKNOWN:
        default: return "未知";
    }
}

// Helper function to print bus type
const char* get_bus_type_str(BusType type) {
    switch (type) {
        case BUS_TYPE_ATA: return "ATA (SATA/PATA)";
        case BUS_TYPE_SCSI: return "SCSI (SAS/USB-SCSI Bridge)";
        case BUS_TYPE_USB: return "USB";
        case BUS_TYPE_NVME: return "NVMe";
        case BUS_TYPE_MMC: return "MMC";
        case BUS_TYPE_VIRTIO: return "Virtio";
        case BUS_TYPE_UNKNOWN:
        default: return "未知";
    }
}

void print_device_info(const DeviceInfo* info) {
    printf("--- 设备信息 (%s) (主设备: %s) ---\n", info->dev_path, info->main_dev_name);
    printf("类型: %s\n", get_device_type_str(info->type));
    printf("接口类型: %s\n", get_bus_type_str(info->bus_type));
    printf("总容量: %.2f GB\n", info->capacity_gb);
    // Print nominal capacity if available
    if (strlen(info->nominal_capacity_str) > 0) {
        printf("标称容量: %s\n", info->nominal_capacity_str);
    }
    printf("扇区数: %llu\n", info->total_sectors);
    printf("逻辑块大小: %u bytes\n", info->logical_block_size);
    printf("物理块大小: %u bytes\n", info->physical_block_size);
    printf("型号: %s\n", strlen(info->model) > 0 ? info->model : "未知");
    printf("厂商: %s\n", strlen(info->vendor) > 0 ? info->vendor : "未知");
    printf("序列号: %s\n", strlen(info->serial) > 0 ? info->serial : "未知");
    printf("固件版本: %s\n", strlen(info->firmware_rev) > 0 ? info->firmware_rev : "未知");
    if (info->type == DEVICE_TYPE_HDD && info->rotation_rate_rpm > 0) {
        printf("转速: %d RPM\n", info->rotation_rate_rpm);
    }
    printf("---\n");
}

// initialize_device_info (unchanged from previous iterations)
void initialize_device_info(DeviceInfo* info, const char* dev_path) {
    memset(info, 0, sizeof(DeviceInfo));
    strncpy(info->dev_path, dev_path, sizeof(info->dev_path) - 1);
    info->dev_path[sizeof(info->dev_path) - 1] = '\0';
    info->bus_type = BUS_TYPE_UNKNOWN;
    info->type = DEVICE_TYPE_UNKNOWN;
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <设备路径>\n", argv[0]);
        return 1;
    }

    const char* dev_path = argv[1];
    DeviceInfo info;
    initialize_device_info(&info, dev_path);

    // Get main device name (e.g., sda from /dev/sda1)
    char* main_dev = get_main_device_name(dev_path);
    if (main_dev) {
        strncpy(info.main_dev_name, main_dev, sizeof(info.main_dev_name) - 1);
        info.main_dev_name[sizeof(info.main_dev_name) - 1] = '\0';
        free(main_dev);
    } else {
        fprintf(stderr, "错误: 无法获取主设备名 (%s).\n", dev_path);
        return 1;
    }

    // Populate info from udevadm
    populate_device_info_from_udevadm(&info);

    // Populate info from sysfs
    populate_device_info_from_sysfs(&info);

    // Run smartctl and populate info from its output
    char* smartctl_output = run_smartctl(dev_path);
    if (smartctl_output) {
        populate_device_info_from_smartctl_output(&info, smartctl_output);
        free(smartctl_output);
    } else {
        fprintf(stderr, "警告: 无法运行 smartctl 或获取其输出。部分信息可能缺失。\n");
    }
    
    print_device_info(&info);

    return 0;
}
