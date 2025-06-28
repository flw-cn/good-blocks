// device_info.c
#include "device_info.h"
#include <stdio.h>
#include <string.h> // For memset and strncpy

// Helper function to print device type
const char* get_device_type_str(DeviceType type) {
    switch (type) {
        case DEVICE_TYPE_HDD: return "HDD";
        case DEVICE_TYPE_SATA_SSD: return "SATA SSD";
        case DEVICE_TYPE_NVME_SSD: return "NVMe SSD";
        case DEVICE_TYPE_USB_STORAGE: return "USB Storage";
        case DEVICE_TYPE_UNKNOWN_SSD: return "未知 SSD"; // Updated string for clarity
        case DEVICE_TYPE_UNKNOWN:
        default: return "未知";
    }
}

// Helper function to print bus type
const char* get_bus_type_str(BusType type) {
    switch (type) {
        case BUS_TYPE_SATA: return "SATA";
        case BUS_TYPE_PATA: return "PATA";
        case BUS_TYPE_SCSI: return "SCSI (SAS/USB-SCSI)";
        case BUS_TYPE_USB: return "USB";
        case BUS_TYPE_NVME: return "NVMe";
        case BUS_TYPE_MMC: return "MMC";
        case BUS_TYPE_VIRTIO: return "Virtio";
        case BUS_TYPE_ATA: return "ATA (General)"; // General ATA fallback
        case BUS_TYPE_UNKNOWN:
        default: return "未知";
    }
}

// Initializes a DeviceInfo struct
void initialize_device_info(DeviceInfo* info, const char* dev_path) {
    memset(info, 0, sizeof(DeviceInfo));
    strncpy(info->dev_path, dev_path, sizeof(info->dev_path) - 1);
    info->dev_path[sizeof(info->dev_path) - 1] = '\0';
    info->bus_type = BUS_TYPE_UNKNOWN;
    info->type = DEVICE_TYPE_UNKNOWN;
}

// Prints the collected device information
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
