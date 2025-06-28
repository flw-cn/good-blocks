// device_info.h
#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#define MAX_DEV_PATH_LEN 32
#define MAX_DEV_NAME_LEN 16
#define MAX_MODEL_LEN 64
#define MAX_VENDOR_LEN 32
#define MAX_SERIAL_LEN 32
#define MAX_FW_REV_LEN 16
#define MAX_FULL_PATH_LEN 1024 // Adjusted based on previous discussions
#define MAX_NOMINAL_CAPACITY_LEN 16

typedef enum {
    BUS_TYPE_UNKNOWN,
    // Add specific SATA and PATA types
    BUS_TYPE_SATA,
    BUS_TYPE_PATA,
    BUS_TYPE_SCSI,      // Includes SAS and USB-SCSI bridges
    BUS_TYPE_USB,
    BUS_TYPE_NVME,
    BUS_TYPE_MMC,
    BUS_TYPE_VIRTIO,
    // Keep a general ATA for fallback if specific type isn't found
    BUS_TYPE_ATA // General ATA, used as a fallback if SATA/PATA cannot be determined
} BusType;

typedef enum {
    DEVICE_TYPE_UNKNOWN,
    DEVICE_TYPE_HDD,
    DEVICE_TYPE_SATA_SSD,
    DEVICE_TYPE_NVME_SSD,
    DEVICE_TYPE_USB_STORAGE,
    DEVICE_TYPE_UNKNOWN_SSD // For SSDs that aren't SATA or NVME (e.g., some SCSI SSDs)
} DeviceType;

typedef struct {
    char dev_path[MAX_DEV_PATH_LEN];        // e.g., /dev/sda
    char main_dev_name[MAX_DEV_NAME_LEN];   // e.g., sda
    DeviceType type;
    BusType bus_type;
    double capacity_gb;                     // Calculated from total_sectors * logical_block_size
    unsigned long long total_sectors;
    unsigned int logical_block_size;
    unsigned int physical_block_size;
    char model[MAX_MODEL_LEN];
    char vendor[MAX_VENDOR_LEN];
    char serial[MAX_SERIAL_LEN];
    char firmware_rev[MAX_FW_REV_LEN];
    int rotation_rate_rpm;                  // 0 for SSDs
    char nominal_capacity_str[MAX_NOMINAL_CAPACITY_LEN]; // New: e.g., "16.0 TB" or "1.02 TB"
} DeviceInfo;

// Function prototypes
void initialize_device_info(DeviceInfo* info, const char* dev_path);
// print_device_info is moved to device_info.c
void print_device_info(const DeviceInfo* info); 

#endif // DEVICE_INFO_H
