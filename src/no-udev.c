#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>     // For readlink, access
#include <sys/sysmacros.h> // For major(), minor(), makedev()
#include <libgen.h>     // For dirname() and basename()

// --- 辅助函数和定义 ---

#ifndef PATH_MAX
#define PATH_MAX 4096 // Fallback if PATH_MAX is not defined
#endif

// 为 PATH_MAX 加上足够的额外空间，以容纳后缀，例如 "/dev", "/device", "/partition"
#define MAX_FULL_PATH_LEN (PATH_MAX + 20)
#define MAX_BUFFER_LEN 128 // 用于读取文件内容的缓冲区

// Helper function to read a sysfs attribute
char* read_sysfs_attribute(const char* full_path) {
    FILE* fp = fopen(full_path, "r");
    if (!fp) {
        // perror("Failed to open sysfs attribute"); // Uncomment for debugging
        return NULL;
    }
    char buffer[MAX_BUFFER_LEN];
    if (fgets(buffer, sizeof(buffer), fp) != NULL) {
        fclose(fp);
        buffer[strcspn(buffer, "\n")] = 0; // Remove trailing newline
        return strdup(buffer); // Return dynamically allocated string, caller must free
    }
    fclose(fp);
    return NULL;
}

/**
 * @brief 从任意块设备路径（可以是分区或整个磁盘）获取其对应的主设备名（即整个磁盘的名称）。
 * 此版本不依赖 libudev，通过 sysfs 目录结构和设备号来确定。
 * @param dev_path 块设备路径，如 "/dev/nvme0n1p5" 或 "/dev/sda1" 或 "/dev/sdb"
 * @return 成功返回主设备名字符串（需调用 free 释放），失败返回 NULL。
 */
char* get_main_device_name_robust_no_udev(const char* dev_path) {
    struct stat st;
    if (stat(dev_path, &st) == -1) {
        perror("stat failed");
        return NULL;
    }

    if (!S_ISBLK(st.st_mode)) {
        fprintf(stderr, "%s 不是一个块设备。\n", dev_path);
        return NULL;
    }

    // 获取设备的主设备号和次设备号
    unsigned int target_major = major(st.st_rdev);
    unsigned int target_minor = minor(st.st_rdev);

    // 1. 使用 /sys/dev/block/<major>:<minor> 找到设备的真实 sysfs 路径
    char sysfs_dev_num_path[MAX_FULL_PATH_LEN];
    snprintf(sysfs_dev_num_path, sizeof(sysfs_dev_num_path), "/sys/dev/block/%u:%u", target_major, target_minor);

    char resolved_sysfs_path_buffer[MAX_FULL_PATH_LEN]; // 用于 readlink 的缓冲区
    ssize_t len = readlink(sysfs_dev_num_path, resolved_sysfs_path_buffer, sizeof(resolved_sysfs_path_buffer) - 1);

    if (len == -1) {
        perror("readlink /sys/dev/block failed");
        fprintf(stderr, "错误：无法为 %s (%u:%u) 找到规范的 sysfs 路径。\n", dev_path, target_major, target_minor);
        return NULL;
    }
    resolved_sysfs_path_buffer[len] = '\0'; // 确保字符串以空字符结尾

    // resolved_sysfs_path_buffer 现在指向类似：
    // 对于 /dev/sda:   "../../../devices/pci0000:00/0000:00:1f.2/ata1/host0/target0:0:0/0:0:0:0/block/sda"
    // 对于 /dev/sda1:  "../../../devices/pci0000:00/0000:00:1f.2/ata1/host0/target0:0:0/0:0:0:0/block/sda/sda1"
    // 对于 /dev/nvme0n1p1: "../../../devices/pci0000:00/0000:00:1d.0/0000:01:00.0/nvme/nvme0/nvme0n1/nvme0n1p1"

    // 2. 从解析后的 sysfs 路径中回溯，找到在 /sys/block/ 中有条目的主设备名
    char current_check_path[MAX_FULL_PATH_LEN];
    char temp_path[MAX_FULL_PATH_LEN]; // 用于 dirname() 和 basename() 的临时缓冲区

    // 复制resolved_sysfs_path_buffer到current_check_path，用于循环中的修改
    strncpy(current_check_path, resolved_sysfs_path_buffer, sizeof(current_check_path) - 1);
    current_check_path[sizeof(current_check_path) - 1] = '\0';

    char* main_dev_name = NULL;

    // 循环直到路径到达根目录或找到主设备
    while (strlen(current_check_path) > 0 && strcmp(current_check_path, "/") != 0) {
        // 获取当前路径的 basename（即目录名或文件名）
        strncpy(temp_path, current_check_path, sizeof(temp_path) - 1); // 复制以便 basename 修改
        temp_path[sizeof(temp_path) - 1] = '\0';
        char* current_base_name = basename(temp_path); // 例如 "sda1", "sda", "nvme0n1p5", "nvme0n1"

        if (strlen(current_base_name) == 0 || strcmp(current_base_name, ".") == 0 || strcmp(current_base_name, "..") == 0) {
            // 避免处理空字符串或 . .. 目录
            strncpy(temp_path, current_check_path, sizeof(temp_path) - 1); // 再次复制以便 dirname 修改
            temp_path[sizeof(temp_path) - 1] = '\0';
            char* parent_dir = dirname(temp_path);
            if (strcmp(parent_dir, current_check_path) == 0) break; // 已经到根目录
            strncpy(current_check_path, parent_dir, sizeof(current_check_path) - 1);
            current_check_path[sizeof(current_check_path) - 1] = '\0';
            continue;
        }

        // 构建 /sys/block/<current_base_name> 的路径
        char sys_block_entry_path[MAX_FULL_PATH_LEN];
        snprintf(sys_block_entry_path, sizeof(sys_block_entry_path), "/sys/block/%s", current_base_name);

        // 检查 /sys/block/ 下是否存在该名称的目录
        struct stat sys_block_stat;
        if (stat(sys_block_entry_path, &sys_block_stat) == 0 && S_ISDIR(sys_block_stat.st_mode)) {
            // 如果存在且是目录，那么我们找到了主设备！
            main_dev_name = strdup(current_base_name);
            break;
        }

        // 如果不是主设备，则向上移动到父目录
        strncpy(temp_path, current_check_path, sizeof(temp_path) - 1); // 再次复制以便 dirname 修改
        temp_path[sizeof(temp_path) - 1] = '\0';
        char* parent_dir = dirname(temp_path); // 例如从 ".../sda/sda1" -> ".../sda"

        // 如果 parent_dir 和 current_check_path 相同，表示已经到根目录或无法再向上
        if (strcmp(parent_dir, current_check_path) == 0) {
            break;
        }
        strncpy(current_check_path, parent_dir, sizeof(current_check_path) - 1);
        current_check_path[sizeof(current_check_path) - 1] = '\0';
    }

    if (!main_dev_name) {
        fprintf(stderr, "错误：遍历 sysfs 路径 '%s' 失败，未能找到对应的主设备。\n", resolved_sysfs_path_buffer);
    }
    return main_dev_name;
}


// get_device_info_robust_no_udev 函数：修改以获取更多信息和更准确的类型判断
void get_device_info_robust_no_udev(const char* dev_path) {
    char* main_dev_name = get_main_device_name_robust_no_udev(dev_path);
    if (!main_dev_name) {
        fprintf(stderr, "错误：无法确定 %s 的主设备名称。\n", dev_path);
        return;
    }

    // 构建主设备在 /sys/block/ 下的路径，用于获取通用硬件信息
    char sysfs_base_path[MAX_FULL_PATH_LEN];
    snprintf(sysfs_base_path, sizeof(sysfs_base_path), "/sys/block/%s", main_dev_name);

    // 额外检查，确保 /sys/block/main_dev_name 实际存在
    if (access(sysfs_base_path, F_OK) != 0) {
        fprintf(stderr, "警告：主设备 sysfs 路径 '%s' 不存在，可能无法获取完整信息。\n", sysfs_base_path);
        free(main_dev_name);
        return;
    }

    printf("--- 设备信息 (%s) (主设备: %s) ---\n", dev_path, main_dev_name);

    char path_buf[MAX_FULL_PATH_LEN];
    char* attr_value = NULL;
    unsigned long long sector_count = 0;
    unsigned long long logical_block_size = 0;
    unsigned long long physical_block_size = 0;

    // 获取扇区数
    snprintf(path_buf, sizeof(path_buf), "%s/size", sysfs_base_path);
    attr_value = read_sysfs_attribute(path_buf);
    if (attr_value) {
        printf("扇区数: %s\n", attr_value);
        sector_count = strtoull(attr_value, NULL, 10);
        free(attr_value);
    }

    // 获取逻辑块大小 (扇区大小)
    snprintf(path_buf, sizeof(path_buf), "%s/queue/logical_block_size", sysfs_base_path);
    attr_value = read_sysfs_attribute(path_buf);
    if (attr_value) {
        logical_block_size = strtoull(attr_value, NULL, 10);
        free(attr_value);
    }

    // 获取物理块大小
    snprintf(path_buf, sizeof(path_buf), "%s/queue/physical_block_size", sysfs_base_path);
    attr_value = read_sysfs_attribute(path_buf);
    if (attr_value) {
        physical_block_size = strtoull(attr_value, NULL, 10);
        free(attr_value);
    }

    // 计算总容量 (GB)
    if (sector_count > 0 && logical_block_size > 0) {
        double total_bytes = (double)sector_count * logical_block_size;
        double total_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
        printf("总容量: %.2f GB\n", total_gb);
    }


    // 获取设备类型：HDD, SSD, USB
    // 检查是否为 NVMe (名称以 nvme 开头)
    if (strncmp(main_dev_name, "nvme", 4) == 0) {
        printf("类型: NVMe SSD\n");
        // 为 NVMe 设备尝试获取特有的信息
        char nvme_sysfs_path[MAX_FULL_PATH_LEN];
        // NVMe 设备的属性通常在 /sys/class/nvme/nvme<ID>/nvme<ID>n<ID>/
        // 或者 /sys/devices/.../nvme/nvme<ID>/nvme<ID>n<ID>/ 下，
        // 我们需要找到它在 /sys/class/nvme/ 下的准确路径。
        // 最简单的方法是根据 main_dev_name 直接构造 /sys/class/nvme/nvme0/nvme0n1/ 这样的路径。
        // 假设 main_dev_name 是 "nvme0n1"
        char nvme_ctrl_name[MAX_BUFFER_LEN]; // "nvme0"
        char* n_pos = strchr(main_dev_name, 'n'); // 找到 'n' 的位置
        if (n_pos) {
            strncpy(nvme_ctrl_name, main_dev_name, n_pos - main_dev_name);
            nvme_ctrl_name[n_pos - main_dev_name] = '\0';
        } else {
            strncpy(nvme_ctrl_name, main_dev_name, sizeof(nvme_ctrl_name)-1);
            nvme_ctrl_name[sizeof(nvme_ctrl_name)-1] = '\0';
        }

        snprintf(nvme_sysfs_path, sizeof(nvme_sysfs_path), "/sys/class/nvme/%s/%s", nvme_ctrl_name, main_dev_name);

        snprintf(path_buf, sizeof(path_buf), "%s/model", nvme_sysfs_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) {
            printf("型号: %s\n", attr_value);
            free(attr_value);
        }

        snprintf(path_buf, sizeof(path_buf), "%s/firmware_rev", nvme_sysfs_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) {
            printf("固件版本: %s\n", attr_value);
            free(attr_value);
        }

        snprintf(path_buf, sizeof(path_buf), "%s/serial", nvme_sysfs_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) {
            printf("序列号: %s\n", attr_value);
            free(attr_value);
        }
        // NVMe 厂商信息通常在控制器级别 /sys/class/nvme/nvme<ID>/vendor
        snprintf(path_buf, sizeof(path_buf), "/sys/class/nvme/%s/vendor", nvme_ctrl_name);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) {
            printf("厂商: %s\n", attr_value);
            free(attr_value);
        } else { // 尝试从 /sys/class/nvme/nvme<ID>/device/vendor
            snprintf(path_buf, sizeof(path_buf), "/sys/class/nvme/%s/device/vendor", nvme_ctrl_name);
            attr_value = read_sysfs_attribute(path_buf);
             if (attr_value) {
                printf("厂商: %s\n", attr_value);
                free(attr_value);
            }
        }


    } else { // 非 NVMe 设备 (sdX, mmcblkX 等)
        snprintf(path_buf, sizeof(path_buf), "%s/queue/rotational", sysfs_base_path);
        char* rotational_str = read_sysfs_attribute(path_buf);

        // 检查是否为 USB 设备
        char usb_id_vendor_path[MAX_FULL_PATH_LEN];
        snprintf(usb_id_vendor_path, sizeof(usb_id_vendor_path), "%s/device/idVendor", sysfs_base_path);
        char* usb_id_vendor = read_sysfs_attribute(usb_id_vendor_path);

        char usb_bus_path[MAX_FULL_PATH_LEN]; // /sys/block/sda/device/uevent 或者 /sys/block/sda/device/usb_device/...
        snprintf(usb_bus_path, sizeof(usb_bus_path), "%s/device/uevent", sysfs_base_path);
        char* uevent_content = read_sysfs_attribute(usb_bus_path);

        int is_usb = 0;
        if (usb_id_vendor != NULL) { // 有 idVendor 通常就是 USB 设备
            is_usb = 1;
            free(usb_id_vendor);
        } else if (uevent_content != NULL && strstr(uevent_content, "SUBSYSTEM=usb") != NULL) {
            is_usb = 1;
        }

        if (is_usb) {
            printf("类型: USB 存储设备\n");
        } else if (rotational_str && strcmp(rotational_str, "0") == 0) {
            printf("类型: SATA/SAS SSD\n");
        } else if (rotational_str && strcmp(rotational_str, "1") == 0) {
            printf("类型: HDD\n");
        } else {
            printf("类型: 未知\n");
        }

        if (rotational_str) free(rotational_str);
        if (uevent_content) free(uevent_content);


        // 获取通用磁盘信息 (对 SATA/USB/MMC 都适用，如果存在的话)
        snprintf(path_buf, sizeof(path_buf), "%s/device/model", sysfs_base_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) {
            printf("型号: %s\n", attr_value);
            free(attr_value);
        } else { // 尝试从 /sys/block/<dev>/device/name (部分设备使用这个)
            snprintf(path_buf, sizeof(path_buf), "%s/device/name", sysfs_base_path);
            attr_value = read_sysfs_attribute(path_buf);
            if (attr_value) {
                printf("型号/名称: %s\n", attr_value);
                free(attr_value);
            }
        }

        snprintf(path_buf, sizeof(path_buf), "%s/device/vendor", sysfs_base_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) {
            printf("厂商: %s\n", attr_value);
            free(attr_value);
        }

        snprintf(path_buf, sizeof(path_buf), "%s/device/serial", sysfs_base_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) {
            printf("序列号: %s\n", attr_value);
            free(attr_value);
        }

        snprintf(path_buf, sizeof(path_buf), "%s/device/rev", sysfs_base_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) {
            printf("固件版本: %s\n", attr_value);
            free(attr_value);
        }
    }


    printf("---\n");

    free(main_dev_name);
}

// --- 辅助函数和定义结束 ---

// main 函数
int main(int argc, const char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <设备路径1> [<设备路径2> ...]\n", argv[0]);
        fprintf(stderr, "示例: %s /dev/nvme0n1p5 /dev/sda /dev/mmcblk0p1\n", argv[0]);
        return 1;
    }

    for(int i = 1; i < argc; i++) {
        get_device_info_robust_no_udev(argv[i]);
        if (i < argc - 1) {
            printf("\n"); // 在不同设备信息块之间添加空行以提高可读性
        }
    }

    return 0;
}
