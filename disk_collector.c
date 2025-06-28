// disk_collector.c
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

static char* read_sysfs_attribute_then_copy(const char* base_path, const char* attr_sub_path, char* dest_buffer, size_t dest_size);

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

// Helper function to execute a command and capture a specific "KEY: VALUE" line
// Returns dynamically allocated string, caller must free.
// Takes KEY (e.g., "Rotation Rate:") and search_key (e.g., "Rotation Rate") for matching
static char* run_command_and_parse_key_value(const char* command, const char* key_prefix, const char* value_start_char_filter) {
    FILE *fp;
    char line[MAX_BUFFER_LEN];
    char* result_str = NULL;

    fp = popen(command, "r");
    if (fp == NULL) {
        // fprintf(stderr, "Warning: Failed to run command: %s\n", command);
        return NULL;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char* prefix_pos = strstr(line, key_prefix);
        if (prefix_pos != NULL) {
            char* value_start = prefix_pos + strlen(key_prefix);
            // Skip leading whitespace characters after the prefix
            while (*value_start && isspace(*value_start)) {
                value_start++;
            }
            
            // Find the end of the value (before newlines or other delimiters)
            char* value_end = value_start;
            while (*value_end && *value_end != '\n' && (*value_start == '\0' || strchr(value_start_char_filter, *value_end) != NULL || isalnum(*value_end) || ispunct(*value_end))) {
                 // Check if the current character is in the filter list or is alphanumeric/punctuation
                 // This ensures we capture the whole value. Filter is for characters we expect in the value, e.g. digits for RPM
                 if (strlen(value_start_char_filter) > 0 && strchr(value_start_char_filter, *value_end) == NULL && !isalnum(*value_end) && !ispunct(*value_end) && !isspace(*value_end)) {
                     break; // Stop if unexpected char
                 }
                value_end++;
            }
            // Remove trailing spaces
            while (value_end > value_start && isspace(*(value_end - 1))) {
                value_end--;
            }

            if (value_end > value_start) {
                size_t len = value_end - value_start;
                result_str = (char*)malloc(len + 1);
                if (result_str) {
                    strncpy(result_str, value_start, len);
                    result_str[len] = '\0';
                }
                break; // Found the value, exit the loop
            }
        }
    }

    pclose(fp);
    return result_str; // Caller must free this memory
}


// Gets the main device name (e.g., sda from /dev/sda1, or nvme0n1 from /dev/nvme0n1p5)
char* get_main_device_name(const char* dev_path) {
    struct stat st;
    if (stat(dev_path, &st) == -1) {
        // perror("stat failed"); // Be silent if stat fails for non-block devices
        return NULL;
    }

    if (!S_ISBLK(st.st_mode)) {
        // fprintf(stderr, "%s 不是一个块设备。\n", dev_path); // Be silent for non-block devices
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


// Populates DeviceInfo from sysfs (primary source) and udevadm (for bus type)
void populate_device_info_from_sysfs(DeviceInfo* info) {
    char sysfs_base_path[MAX_FULL_PATH_LEN];
    snprintf(sysfs_base_path, sizeof(sysfs_base_path), "/sys/block/%s", info->main_dev_name);

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

    // --- Determine Bus Type using udevadm (more reliable) ---
    char udevadm_cmd[MAX_FULL_PATH_LEN + 64];
    snprintf(udevadm_cmd, sizeof(udevadm_cmd), "udevadm info --query=property --name=%s", info->dev_path);
    
    char* bus_type_str = run_command_and_parse_key_value(udevadm_cmd, "ID_BUS=", ""); // ID_BUS value is usually alpha
    if (bus_type_str) {
        if (strcmp(bus_type_str, "ata") == 0) info->bus_type = BUS_TYPE_ATA;
        else if (strcmp(bus_type_str, "scsi") == 0) info->bus_type = BUS_TYPE_SCSI;
        else if (strcmp(bus_type_str, "usb") == 0) info->bus_type = BUS_TYPE_USB;
        else if (strcmp(bus_type_str, "nvme") == 0) info->bus_type = BUS_TYPE_NVME;
        else if (strcmp(bus_type_str, "mmc") == 0) info->bus_type = BUS_TYPE_MMC;
        else if (strcmp(bus_type_str, "virtio") == 0) info->bus_type = BUS_TYPE_VIRTIO;
        else info->bus_type = BUS_TYPE_UNKNOWN;
        free(bus_type_str);
    }

    // --- Determine Device Type and get type-specific info ---
    if (info->bus_type == BUS_TYPE_NVME || strncmp(info->main_dev_name, "nvme", 4) == 0) {
        info->type = DEVICE_TYPE_NVME_SSD;
        // NVMe specific paths for model, serial, firmware_rev, vendor
        char nvme_ctrl_name[MAX_BUFFER_LEN];
        char* n_pos = strchr(info->main_dev_name, 'n');
        if (n_pos) {
            strncpy(nvme_ctrl_name, info->main_dev_name, n_pos - info->main_dev_name);
            nvme_ctrl_name[n_pos - info->main_dev_name] = '\0';
        } else {
            strncpy(nvme_ctrl_name, info->main_dev_name, sizeof(nvme_ctrl_name)-1);
            nvme_ctrl_name[sizeof(nvme_ctrl_name)-1] = '\0';
        }

        char nvme_class_path[MAX_FULL_PATH_LEN];
        snprintf(nvme_class_path, sizeof(nvme_class_path), "/sys/class/nvme/%s/%s", nvme_ctrl_name, info->main_dev_name);

        attr_value = read_sysfs_attribute_then_copy(nvme_class_path, "model", info->model, sizeof(info->model));
        if (attr_value) free(attr_value);

        attr_value = read_sysfs_attribute_then_copy(nvme_class_path, "firmware_rev", info->firmware_rev, sizeof(info->firmware_rev));
        if (attr_value) free(attr_value);

        attr_value = read_sysfs_attribute_then_copy(nvme_class_path, "serial", info->serial, sizeof(info->serial));
        if (attr_value) free(attr_value);

        snprintf(path_buf, sizeof(path_buf), "/sys/class/nvme/%s/vendor", nvme_ctrl_name);
        attr_value = read_sysfs_attribute(path_buf);
        if (attr_value) { strncpy(info->vendor, attr_value, sizeof(info->vendor)-1); free(attr_value); }
        else { // Fallback for vendor
            snprintf(path_buf, sizeof(path_buf), "/sys/class/nvme/%s/device/vendor", nvme_ctrl_name);
            attr_value = read_sysfs_attribute(path_buf);
            if (attr_value) { strncpy(info->vendor, attr_value, sizeof(info->vendor)-1); free(attr_value); }
        }

    } else { // Non-NVMe devices (sdX, mmcblkX etc.)
        char* rotational_str = NULL;
        snprintf(path_buf, sizeof(path_buf), "%s/queue/rotational", sysfs_base_path);
        rotational_str = read_sysfs_attribute(path_buf);
        
        // Refined device type based on rotational property and bus type
        if (info->bus_type == BUS_TYPE_USB) {
            info->type = DEVICE_TYPE_USB_STORAGE;
        } else if (rotational_str && strcmp(rotational_str, "0") == 0) {
            info->type = DEVICE_TYPE_SATA_SSD;
        } else if (rotational_str && strcmp(rotational_str, "1") == 0) {
            info->type = DEVICE_TYPE_HDD;
        } else {
            info->type = DEVICE_TYPE_UNKNOWN;
        }

        if (rotational_str) free(rotational_str);

        // Get common attributes for SATA/USB/MMC, etc. (if not already found by NVMe path)
        // Add a helper for common sysfs reads to simplify
        if (strlen(info->model) == 0) { // Only read if not already populated by NVMe branch
            attr_value = read_sysfs_attribute_then_copy(sysfs_base_path, "device/model", info->model, sizeof(info->model));
            if (attr_value) free(attr_value);
            else { // Fallback for name
                attr_value = read_sysfs_attribute_then_copy(sysfs_base_path, "device/name", info->model, sizeof(info->model));
                if (attr_value) free(attr_value);
            }
        }

        if (strlen(info->vendor) == 0) {
            attr_value = read_sysfs_attribute_then_copy(sysfs_base_path, "device/vendor", info->vendor, sizeof(info->vendor));
            if (attr_value) free(attr_value);
        }

        if (strlen(info->serial) == 0) {
            attr_value = read_sysfs_attribute_then_copy(sysfs_base_path, "device/serial", info->serial, sizeof(info->serial));
            if (attr_value) free(attr_value);
        }

        if (strlen(info->firmware_rev) == 0) {
            attr_value = read_sysfs_attribute_then_copy(sysfs_base_path, "device/rev", info->firmware_rev, sizeof(info->firmware_rev));
            if (attr_value) free(attr_value);
        }
    }
}

// Helper to encapsulate common attribute reading from sysfs for clarity
static char* read_sysfs_attribute_then_copy(const char* base_path, const char* attr_sub_path, char* dest_buffer, size_t dest_size) {
    char full_path[MAX_FULL_PATH_LEN];
    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, attr_sub_path);
    char* attr_value = read_sysfs_attribute(full_path);
    if (attr_value) {
        strncpy(dest_buffer, attr_value, dest_size - 1);
        dest_buffer[dest_size - 1] = '\0';
    }
    return attr_value; // Still return this so caller can free it
}


// Populates DeviceInfo from smartctl (for RPM and serial number fallback)
void populate_device_info_from_smartctl(DeviceInfo* info) {
    char smartctl_cmd[MAX_FULL_PATH_LEN + 32]; // smartctl -a /dev/device
    snprintf(smartctl_cmd, sizeof(smartctl_cmd), "smartctl -a %s", info->dev_path);

    // Get RPM if it's an HDD
    if (info->type == DEVICE_TYPE_HDD && strlen(info->rotation_rate) == 0) { // Only try if not already found (unlikely for RPM)
        char* rpm_value = run_command_and_parse_key_value(smartctl_cmd, "Rotation Rate:", "0123456789");
        if (rpm_value) {
            strncpy(info->rotation_rate, rpm_value, sizeof(info->rotation_rate)-1);
            free(rpm_value);
        }
    }

    // Get Serial Number if sysfs didn't provide it
    if (strlen(info->serial) == 0) {
        char* serial_value = run_command_and_parse_key_value(smartctl_cmd, "Serial Number:", ""); // Serial can be alphanumeric
        if (!serial_value) { // Try "Serial:" as fallback for some older drives
            serial_value = run_command_and_parse_key_value(smartctl_cmd, "Serial:", "");
        }
        if (serial_value) {
            strncpy(info->serial, serial_value, sizeof(info->serial)-1);
            free(serial_value);
        }
    }
    // You could extend for model/vendor/firmware if sysfs also misses them.
    // For now, sticking to serial as requested.
}
