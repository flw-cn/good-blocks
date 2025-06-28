#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <stddef.h> // For size_t

// Define PATH_MAX and MAX_BUFFER_LEN here as well, if they are not system-wide
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_BUFFER_LEN 128

#define MAX_FULL_PATH_LEN 4096

// Device type enum
typedef enum {
    DEVICE_TYPE_UNKNOWN,
    DEVICE_TYPE_HDD,
    DEVICE_TYPE_SATA_SSD,
    DEVICE_TYPE_NVME_SSD,
    DEVICE_TYPE_USB_STORAGE
} DeviceType;

// DeviceInfo structure
typedef struct {
    char dev_path[PATH_MAX];       // e.g., /dev/sda, /dev/nvme0n1p5
    char main_dev_name[MAX_BUFFER_LEN]; // e.g., sda, nvme0n1

    DeviceType type;               // Detected device type

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
    
    // Form Factor is not commonly available in sysfs in a clean way for all devices.
    // It's often in smartctl output or needs specific parsing.
    // For now, we omit it to keep sysfs parsing simpler.

    // Flags for data source/presence
    int sysfs_model_found;
    int sysfs_vendor_found;
    int sysfs_serial_found;
    int sysfs_firmware_found;

    int smartctl_rotation_rate_found;
    // Add more flags if other smartctl fields are parsed
} DeviceInfo;

// Function prototypes
void init_device_info(DeviceInfo* info);
void print_device_info(const DeviceInfo* info);

#endif // DEVICE_INFO_H
