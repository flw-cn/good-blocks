// device_info.h
#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#define MAX_DEV_PATH_LEN 256
#define MAX_DEV_NAME_LEN 32
#define MAX_DESC_LEN    128
#define MAX_FULL_PATH_LEN 1024
#define MAX_SYSFS_PATH_LEN (MAX_FULL_PATH_LEN + 64)

typedef enum {
    BUS_TYPE_UNKNOWN,
    BUS_TYPE_SATA,
    BUS_TYPE_PATA,
    BUS_TYPE_SCSI,
    BUS_TYPE_USB,
    BUS_TYPE_NVME,
    BUS_TYPE_MMC,
    BUS_TYPE_VIRTIO,
    BUS_TYPE_ATA
} BusType;

typedef enum {
    DEVICE_TYPE_UNKNOWN,
    DEVICE_TYPE_HDD,
    DEVICE_TYPE_SATA_SSD,
    DEVICE_TYPE_NVME_SSD,
    DEVICE_TYPE_USB_STORAGE,
    DEVICE_TYPE_UNKNOWN_SSD
} DeviceType;

// 设备信息
typedef struct {
    // === 基本设备标识 ===
    char dev_path[MAX_DEV_PATH_LEN];        // 设备路径 /dev/sda
    char main_dev_name[MAX_DEV_NAME_LEN];   // 主设备名 sda

    // === 设备类型信息 ===
    DeviceType device_type;                 // 设备类型枚举
    BusType bus_type;                       // 总线类型枚举
    int is_rotational;                      // 0=SSD, 1=HDD, -1=unknown (兼容原代码)
    int rpm;                                // 每分钟转速，0 表示 SSD 或未知
    int rotation_rate_rpm;                  // 添加这个字段，与 rpm 等价

    // === 容量和几何信息 ===
    double capacity_gb;                     // 计算得出的容量(GB)
    unsigned long long total_sectors;      // 总扇区数
    unsigned int logical_block_size;       // 逻辑块大小
    unsigned int physical_block_size;      // 物理块大小
    unsigned int optimal_io_size;          // 最优I/O大小

    // === 设备详细信息 ===
    char nominal_capacity_str[MAX_DESC_LEN]; // 标称容量字符串 "16.0 TB"
    char model[MAX_DESC_LEN];             // 设备型号 (统一了长度)
    char vendor[MAX_DESC_LEN];           // 厂商信息 (统一了长度)
    char serial[MAX_DESC_LEN];           // 序列号
    char firmware_rev[MAX_DESC_LEN];     // 固件版本

    // === 检测状态信息 ===
    int info_collection_status;            // 信息收集状态：0=成功, 1=部分失败, 2=大部分失败
} DeviceInfo;

// 函数声明
void initialize_device_info(DeviceInfo* info, const char* dev_path);
int collect_device_info(DeviceInfo* info);
void print_device_info(const DeviceInfo* info);

// 类型转换函数
const char* get_device_type_str(const DeviceInfo* info);  // 接受枚举类型
const char* get_bus_type_str(BusType type);
const char* get_device_type_legacy_str(const DeviceInfo* info); // 返回类似 "SSD", "HDD" 的字符串

// 设备特性判断函数
int is_ssd_device(const DeviceInfo* info);
int is_hdd_device(const DeviceInfo* info);
int is_nvme_device(const DeviceInfo* info);
int get_recommended_suspect_threshold(const DeviceInfo* info);

// 信息收集函数（从 disk_collector.c 移植）
char* get_main_device_name(const char* dev_path);
char* run_smartctl(const char* dev_path);
void populate_device_info_from_udevadm(DeviceInfo* info);
void populate_device_info_from_sysfs(DeviceInfo* info);
void populate_device_info_from_smartctl_output(DeviceInfo* info, const char* smartctl_output);

#endif // DEVICE_INFO_H
