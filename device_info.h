// device_info.h
#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <stddef.h> // For size_t
#include <limits.h> // For PATH_MAX

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_BUFFER_LEN 128 // Ensure this is defined appropriately
#define MAX_FULL_PATH_LEN 4096

// Device type enum
typedef enum {
    DEVICE_TYPE_UNKNOWN,
    DEVICE_TYPE_HDD,
    DEVICE_TYPE_SATA_SSD,
    DEVICE_TYPE_NVME_SSD,
    DEVICE_TYPE_USB_STORAGE
} DeviceType;

// Bus type enum
typedef enum {
    BUS_TYPE_UNKNOWN,
    BUS_TYPE_ATA,   // Includes SATA and PATA (IDE)
    BUS_TYPE_SCSI,  // Includes SAS
    BUS_TYPE_USB,
    BUS_TYPE_NVME,
    BUS_TYPE_MMC,   // For SD cards, eMMC
    BUS_TYPE_VIRTIO // For virtual machines
} BusType;


// DeviceInfo structure
typedef struct {
    char dev_path[PATH_MAX];       // e.g., /dev/sda, /dev/nvme0n1p5
    char main_dev_name[MAX_BUFFER_LEN]; // e.g., sda, nvme0n1

    DeviceType type;               // Detected device type (HDD, SSD, USB)
    BusType bus_type;              // Detected bus interface (ATA, USB, NVMe, etc.)

    // Common attributes
    unsigned long long total_sectors; // Total sectors (from sysfs)
    unsigned long long logical_block_size; // Logical sector size (from sysfs)
    unsigned long long physical_block_size; // Physical sector size (from sysfs)
    double capacity_gb;            // Total capacity in GB (calculated)

    char model[MAX_BUFFER_LEN];    // Model string
    char vendor[MAX_BUFFER_LEN];   // Vendor string
    char serial[MAX_BUFFER_LEN];   // Serial number
    char firmware_rev[MAX_BUFFER_LEN]; // Firmware revision

    // HDD specific
    char rotation_rate[MAX_BUFFER_LEN]; // RPM (from smartctl for HDD)
    
} DeviceInfo;

// Function prototypes
void init_device_info(DeviceInfo* info);
void print_device_info(const DeviceInfo* info);

#endif // DEVICE_INFO_Hendif // DEVICE_INFO_H
