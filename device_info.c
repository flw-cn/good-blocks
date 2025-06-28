// device_info.c
#include "device_info.h"
#include <stdio.h>
#include <string.h>

// Initialize DeviceInfo structure with default values
void init_device_info(DeviceInfo* info) {
    memset(info, 0, sizeof(DeviceInfo)); // Zero out the entire structure
    info->type = DEVICE_TYPE_UNKNOWN;
    info->bus_type = BUS_TYPE_UNKNOWN; // Initialize new bus type
    info->rotation_rate_rpm = 0; // Initialize new RPM field
}

// Print DeviceInfo structure content
void print_device_info(const DeviceInfo* info) {
    printf("--- 设备信息 (%s) (主设备: %s) ---\n", info->dev_path, info->main_dev_name);

    printf("类型: ");
    switch (info->type) {
        case DEVICE_TYPE_HDD: printf("HDD\n"); break;
        case DEVICE_TYPE_SATA_SSD: printf("SATA/SAS SSD\n"); break;
        case DEVICE_TYPE_NVME_SSD: printf("NVMe SSD\n"); break;
        case DEVICE_TYPE_USB_STORAGE: printf("USB 存储设备\n"); break;
        case DEVICE_TYPE_UNKNOWN:
        default: printf("未知\n"); break;
    }

    printf("接口类型: ");
    switch (info->bus_type) {
        case BUS_TYPE_ATA: printf("ATA (SATA/PATA)\n"); break;
        case BUS_TYPE_SCSI: printf("SCSI (SAS/USB-SCSI)\n"); break; // USB devices often appear under SCSI subsystem
        case BUS_TYPE_USB: printf("USB\n"); break;
        case BUS_TYPE_NVME: printf("NVMe\n"); break;
        case BUS_TYPE_MMC: printf("MMC/SD\n"); break;
        case BUS_TYPE_VIRTIO: printf("VirtIO (Virtual)\n"); break;
        case BUS_TYPE_UNKNOWN:
        default: printf("未知\n"); break;
    }


    if (info->total_sectors > 0 && info->capacity_gb > 0) {
        printf("总容量: %.2f GB\n", info->capacity_gb);
    } else {
        printf("总容量: 无法获取\n");
    }
    printf("扇区数: %llu\n", info->total_sectors);
    printf("逻辑块大小: %llu bytes\n", info->logical_block_size);
    printf("物理块大小: %llu bytes\n", info->physical_block_size);

    if (strlen(info->model) > 0) {
        printf("型号: %s\n", info->model);
    } else {
        printf("型号: 无法获取\n");
    }

    if (strlen(info->vendor) > 0) {
        printf("厂商: %s\n", info->vendor);
    } else {
        printf("厂商: 无法获取\n");
    }

    if (strlen(info->serial) > 0) {
        printf("序列号: %s\n", info->serial);
    } else {
        printf("序列号: 无法获取\n");
    }

    if (strlen(info->firmware_rev) > 0) {
        printf("固件版本: %s\n", info->firmware_rev);
    } else {
        printf("固件版本: 无法获取\n");
    }

    // Updated: Print integer RPM
    if (info->type == DEVICE_TYPE_HDD && info->rotation_rate_rpm > 0) {
        printf("转速: %d RPM\n", info->rotation_rate_rpm);
    } else if (info->type == DEVICE_TYPE_HDD) {
        printf("转速: 无法获取\n");
    }

    printf("---\n");
}
