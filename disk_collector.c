#include "disk_collector.h"
#include "device_info.h" // Include DeviceInfo structure
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>     // For readlink, access
#include <sys/sysmacros.h> // For major(), minor(), makedev()
#include <libgen.h>     // For dirname() and basename()
#include <ctype.h>      // For isspace(), isdigit()

// Path max and buffer len defines (ensure consistent with device_info.h)
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_BUFFER_LEN 128


// Helper function to read a sysfs attribute file
static char* read_sysfs_attribute(const char* full_path) {
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

// Function to get HDD Rotation Rate using smartctl
static char* get_hdd_rpm_from_smartctl(const char* dev_path) {
    char command[256];
    char line[MAX_BUFFER_LEN];
    FILE *fp;
    char* rpm_str = NULL;

    snprintf(command, sizeof(command), "sudo smartctl -a %s", dev_path);
    fp = popen(command, "r");
    if (fp == NULL) {
        // fprintf(stderr, "Warning: Failed to run smartctl for %s\n", dev_path); // Optionally print warning
        return NULL;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* rotation_rate_prefix = strstr(line, "Rotation Rate:");
        if (rotation_rate_prefix != NULL) {
            char* num_start = rotation_rate_prefix + strlen("Rotation Rate:");
            while (*num_start && isspace(*num_start)) {
                num_start++;
            }
            char* num_end = num_start;
            while (*num_end && isdigit(*num_end)) {
                num_end++;
            }

            if (num_end > num_start) {
                size_t len = num_end - num_start;
                rpm_str = (char*)malloc(len + 1);
                if (rpm_str) {
                    strncpy(rpm_str, num_start, len);
                    rpm_str[len] = '\0';
                }
                break;
            }
        }
    }

    pclose(fp);
    return rpm_str;
}


// Gets the main device name (e.g., sda from /dev/sda1, or nvme0n1 from /dev/nvme0n1p5)
char* get_main_device_name(const char* dev_path) {
    struct stat st;
    if (stat(dev_path, &st) == -1) {
        perror("stat failed");
        return NULL;
    }

    if (!S_ISBLK(st.st_mode)) {
        // fprintf(stderr, "%s 不是一个块设备。\n", dev_path); // Keep silent for non-block devices
        return NULL;
    }

    unsigned int target_major = major(st.st_rdev);
    unsigned int target_minor = minor(st.st_rdev);

    char sysfs_dev_num_path[MAX_FULL_PATH_LEN];
    snprintf(sysfs_dev_num_path, sizeof(sysfs_dev_num_path), "/sys/dev/block/%u:%u", target_major, target_minor);

    char resolved_sysfs_path_buffer[MAX_FULL_PATH_LEN];
    ssize_t len = readlink(sysfs_dev_num_path, resolved_sysfs_path_buffer, sizeof(resolved_sysfs_path_buffer) - 1);

    if (len == -1) {
        // fprintf(stderr, "错误：无法为 %s (%u:%u) 找到规范的 sysfs 路径。\n", dev_path, target_major, target_minor);
        return NULL;
    }
    resolved_sysfs_path_buffer[len] = '\0';

    char current_check_path[MAX_FULL_PATH_LEN];
    char temp_path[MAX_FULL_PATH_LEN];
    
    strncpy(current_check_path, resolved_sysfs_path_buffer, sizeof(current_check_path) - 1);
    current_check_path[sizeof(current_check_path) - 1] = '\0';

    char* main_dev_name_str = NULL;

    while (strlen(current_check_path) > 0 && strcmp(current_check_path, "/") != 0) {
        strncpy(temp_path, current_check_path, sizeof(temp_path) - 1);
        temp_path[sizeof(temp_path) - 1] = '\0';
        char* current_base_name = basename(temp_path);

        if (strlen(current_base_name) == 0 || strcmp(current_base_name, ".") == 0 || strcmp(current_base_name, "..") == 0) {
            strncpy(temp_path, current_check_path, sizeof(temp_path) - 1);
            temp_path[sizeof(temp_path) - 1] = '\0';
            char* parent_dir = dirname(temp_path);
            if (strcmp(parent_dir, current_check_path) == 0) break;
            strncpy(current_check_path, parent_dir, sizeof(current_check_path) - 1);
            current_check_path[sizeof(current_check_path) - 1] = '\0';
            continue;
        }

        char sys_block_entry_path[MAX_FULL_PATH_LEN];
        snprintf(sys_block_entry_path, sizeof(sys_block_entry_path), "/sys/block/%s", current_base_name);

        struct stat sys_block_stat;
        if (stat(sys_block_entry_path, &sys_block_stat) == 0 && S_ISDIR(sys_block_stat.st_mode)) {
            main_dev_name_str = strdup(current_base_name);
            break;
        }

        strncpy(temp_path, current_check_path, sizeof(temp_path) - 1);
        temp_path[sizeof(temp_path) - 1] = '\0';
        char* parent_dir = dirname(temp_path);

        if (strcmp(parent_dir, current_check_path) == 0) {
            break;
        }
        strncpy(current_check_path, parent_dir, sizeof(current_check_path) - 1);
        current_check_path[sizeof(current_check_path) - 1] = '\0';
    }

    return main_dev_name_str;
}


// Populates DeviceInfo from sysfs
void populate_device_info_from_sysfs(DeviceInfo* info) {
    char sysfs_base_path[MAX_FULL_PATH_LEN];
    snprintf(sysfs_base_path, sizeof(sysfs_base_path), "/sys/block/%s", info->main_dev_name);

    // Basic existence check
    if (access(sysfs_base_path, F_OK) != 0) {
        fprintf(stderr, "Warning: Main device sysfs path '%s' not found, limited info.\n", sysfs_base_path);
        return;
    }

    char path_buf[MAX_FULL_PATH_LEN];
    char* attr_value = NULL;

    // Get total sectors
    snprintf(path_buf, sizeof(path_buf), "%s/size", sysfs_base_path);
    attr_value = read_sysfs_attribute(path_buf);
    if (attr_value) {
        info->total_sectors = strtoull(attr_value, NULL, 10);
        free(attr_value);
    }

    // Get logical block size
    snprintf(path_buf, sizeof(path_buf), "%s/queue/logical_block_size", sysfs_base_path);
    attr_value = read_sysfs_attribute(path_buf);
    if (attr_value) {
        info->logical_block_size = strtoull(attr_value, NULL, 10);
        free(attr_value);
    }

    // Get physical block size
    snprintf(path_buf, sizeof(path_buf), "%s/queue/physical_block_size", sysfs_base_path);
    attr_value = read_sysfs_attribute(path_buf);
    if (attr_value) {
        info->physical_block_size = strtoull(attr_value, NULL, 10);
        free(attr_value);
    }
    
    // Calculate total capacity in GB
    if (info->total_sectors > 0 && info->logical_block_size > 0) {
        double total_bytes = (double)info->total_sectors * info->logical_block_size;
        info->capacity_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
    }

    // Determine Device Type and get type-specific info
    if (strncmp(info->main_dev_name, "nvme", 4) == 0) {
        info->type = DEVICE_TYPE_NVME_SSD;
        // NVMe specific paths for model, serial, firmware_rev, vendor
        char nvme_ctrl_name[MAX_BUFFER_LEN];
        char* n_pos = strchr(info->main_dev_name, 'n');
        if (n_pos) {
            strncpy(nvme_ctrl_name, info->main_dev_name, n_pos - info->main_dev_name);
            nvme_ctrl_name[n_pos - info->main_dev_name] = '\0';
        } else { // Should not happen for nvme0n1, but for nvme0 it could
            strncpy(nvme_ctrl_name, info->main_dev_name, sizeof(nvme_ctrl_name)-1);
            nvme_ctrl_name[sizeof(nvme_ctrl_name)-1] = '\0';
        }

        char nvme_class_path[MAX_FULL_PATH_LEN];
        snprintf(nvme_class_path, sizeof(nvme_class_path), "/sys/class/nvme/%s/%s", nvme_ctrl_name, info->main_dev_name);

        snprintf(path_buf, sizeof(path_buf), "%s/model", nvme_class_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) { strncpy(info->model, attr_value, sizeof(info->model)-1); free(attr_value); info->sysfs_model_found = 1; }

        snprintf(path_buf, sizeof(path_buf), "%s/firmware_rev", nvme_class_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) { strncpy(info->firmware_rev, attr_value, sizeof(info->firmware_rev)-1); free(attr_value); info->sysfs_firmware_found = 1; }

        snprintf(path_buf, sizeof(path_buf), "%s/serial", nvme_class_path);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) { strncpy(info->serial, attr_value, sizeof(info->serial)-1); free(attr_value); info->sysfs_serial_found = 1; }

        snprintf(path_buf, sizeof(path_buf), "/sys/class/nvme/%s/vendor", nvme_ctrl_name);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) { strncpy(info->vendor, attr_value, sizeof(info->vendor)-1); free(attr_value); info->sysfs_vendor_found = 1; }
        else { // Fallback for vendor
            snprintf(path_buf, sizeof(path_buf), "/sys/class/nvme/%s/device/vendor", nvme_ctrl_name);
            attr_value = read_sysfs_attribute(path_buf);
            if (attr_value) { strncpy(info->vendor, attr_value, sizeof(info->vendor)-1); free(attr_value); info->sysfs_vendor_found = 1; }
        }

    } else { // Non-NVMe devices (sdX, mmcblkX etc.)
        char* rotational_str = NULL;
        snprintf(path_buf, sizeof(path_buf), "%s/queue/rotational", sysfs_base_path);
        rotational_str = read_sysfs_attribute(path_buf);

        // Check for USB device
        char usb_id_vendor_path[MAX_FULL_PATH_LEN];
        snprintf(usb_id_vendor_path, sizeof(usb_id_vendor_path), "%s/device/idVendor", sysfs_base_path);
        char* usb_id_vendor = read_sysfs_attribute(usb_id_vendor_path);
        
        char uevent_path[MAX_FULL_PATH_LEN];
        snprintf(uevent_path, sizeof(uevent_path), "%s/device/uevent", sysfs_base_path);
        char* uevent_content = read_sysfs_attribute(uevent_path);
        
        int is_usb = 0;
        if (usb_id_vendor != NULL) {
            is_usb = 1;
            free(usb_id_vendor);
        } else if (uevent_content != NULL && strstr(uevent_content, "SUBSYSTEM=usb") != NULL) {
            is_usb = 1;
        }

        if (is_usb) {
            info->type = DEVICE_TYPE_USB_STORAGE;
        } else if (rotational_str && strcmp(rotational_str, "0") == 0) {
            info->type = DEVICE_TYPE_SATA_SSD;
        } else if (rotational_str && strcmp(rotational_str, "1") == 0) {
            info->type = DEVICE_TYPE_HDD;
        } else {
            info->type = DEVICE_TYPE_UNKNOWN;
        }

        if (rotational_str) free(rotational_str);
        if (uevent_content) free(uevent_content);

        // Get common attributes for SATA/USB/MMC, etc.
        if (!info->sysfs_model_found) {
            snprintf(path_buf, sizeof(path_buf), "%s/device/model", sysfs_base_path);
            attr_value = read_sysfs_attribute(path_buf);
            if (attr_value) { strncpy(info->model, attr_value, sizeof(info->model)-1); free(attr_value); info->sysfs_model_found = 1; }
            else { // Fallback for name
                snprintf(path_buf, sizeof(path_buf), "%s/device/name", sysfs_base_path);
                attr_value = read_sysfs_attribute(path_buf);
                if (attr_value) { strncpy(info->model, attr_value, sizeof(info->model)-1); free(attr_value); info->sysfs_model_found = 1; }
            }
        }

        if (!info->sysfs_vendor_found) {
            snprintf(path_buf, sizeof(path_buf), "%s/device/vendor", sysfs_base_path);
            attr_value = read_sysfs_attribute(path_buf);
            if (attr_value) { strncpy(info->vendor, attr_value, sizeof(info->vendor)-1); free(attr_value); info->sysfs_vendor_found = 1; }
        }

        if (!info->sysfs_serial_found) {
            snprintf(path_buf, sizeof(path_buf), "%s/device/serial", sysfs_base_path);
            attr_value = read_sysfs_attribute(path_buf);
            if (attr_value) { strncpy(info->serial, attr_value, sizeof(info->serial)-1); free(attr_value); info->sysfs_serial_found = 1; }
        }

        if (!info->sysfs_firmware_found) {
            snprintf(path_buf, sizeof(path_buf), "%s/device/rev", sysfs_base_path);
            attr_value = read_sysfs_attribute(path_buf);
            if (attr_value) { strncpy(info->firmware_rev, attr_value, sizeof(info->firmware_rev)-1); free(attr_value); info->sysfs_firmware_found = 1; }
        }
    }
}

// Populates DeviceInfo from smartctl (for RPM and potentially other fallback info)
void populate_device_info_from_smartctl(DeviceInfo* info) {
    if (info->type == DEVICE_TYPE_HDD) {
        char* rpm_value = get_hdd_rpm_from_smartctl(info->dev_path);
        if (rpm_value) {
            strncpy(info->rotation_rate, rpm_value, sizeof(info->rotation_rate)-1);
            free(rpm_value);
            info->smartctl_rotation_rate_found = 1;
        }
    }

    // You could extend this to parse more smartctl info if sysfs data is missing
    // e.g., if info->model is empty, try parsing "Device Model:" from smartctl
    // This would make the parsing more complex for different fields, so kept simple for now.
}
