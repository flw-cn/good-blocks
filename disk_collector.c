// disk_collector.c
#include "disk_collector.h"
#include "device_info.h"
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

// Gets the main device name (e.g., sda from /dev/sda1, or nvme0n1 from /dev/nvme0n1p5)
char* get_main_device_name(const char* dev_path) {
    struct stat st;
    if (stat(dev_path, &st) == -1) {
        return NULL;
    }

    if (!S_ISBLK(st.st_mode)) {
        return NULL;
    }

    unsigned int target_major = major(st.st_rdev);
    unsigned int target_minor = minor(st.st_rdev);

    char sysfs_dev_num_path[MAX_FULL_PATH_LEN];
    snprintf(sysfs_dev_num_path, sizeof(sysfs_dev_num_path), "/sys/dev/block/%u:%u", target_major, target_minor);

    char resolved_sysfs_path_buffer[MAX_FULL_PATH_LEN];
    ssize_t len = readlink(sysfs_dev_num_path, resolved_sysfs_path_buffer, sizeof(resolved_sysfs_path_buffer) - 1);

    if (len == -1) {
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


// New function: Runs smartctl and captures its entire output
char* run_smartctl(const char* dev_path) {
    char command[MAX_FULL_PATH_LEN + 32]; // smartctl -a /dev/device
    snprintf(command, sizeof(command), "sudo smartctl -a %s", dev_path);

    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        // fprintf(stderr, "Warning: Failed to run smartctl for %s\n", dev_path);
        return NULL;
    }

    // Read entire output into a dynamically growing buffer
    char* full_output = NULL;
    size_t current_size = 0;
    char buffer[1024]; // Read in chunks

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        size_t buffer_len = strlen(buffer);
        full_output = realloc(full_output, current_size + buffer_len + 1);
        if (full_output == NULL) {
            fprintf(stderr, "Memory allocation failed for smartctl output.\n");
            pclose(fp);
            return NULL;
        }
        strcpy(full_output + current_size, buffer);
        current_size += buffer_len;
    }

    pclose(fp);
    return full_output; // Caller must free this
}

// New function: Parses a string for multiple potential keys (from smartctl or udevadm output)
// This is a more generic parser for "KEY: VALUE" or "KEY=VALUE" lines.
// key_prefix_format: The format string for the key, e.g., "Rotation Rate:", "ID_BUS=", "Serial Number:"
// ...: variable arguments for potential alternative key prefixes (NULL-terminated)
char* get_string_from_output(const char* output_str, const char* key_prefix_format, ...) {
    if (!output_str) return NULL;

    va_list args;
    const char* current_key_prefix;
    char* result_str = NULL;

    // Iterate through all provided key prefixes
    va_start(args, key_prefix_format);
    current_key_prefix = key_prefix_format;

    while (current_key_prefix != NULL) {
        const char* line_start = output_str;
        while (*line_start != '\0') {
            const char* line_end = strchr(line_start, '\n');
            size_t line_len;
            if (line_end == NULL) {
                line_len = strlen(line_start);
                line_end = line_start + line_len; // Point to null terminator
            } else {
                line_len = line_end - line_start;
            }

            // Create a temporary null-terminated string for the current line
            char temp_line[MAX_BUFFER_LEN];
            if (line_len >= sizeof(temp_line)) {
                line_len = sizeof(temp_line) - 1; // Truncate if too long
            }
            strncpy(temp_line, line_start, line_len);
            temp_line[line_len] = '\0';

            char* prefix_pos = strstr(temp_line, current_key_prefix);
            if (prefix_pos != NULL) {
                char* value_start = prefix_pos + strlen(current_key_prefix);
                // Skip leading whitespace after the prefix
                while (*value_start && isspace(*value_start)) {
                    value_start++;
                }
                
                char* value_end = value_start;
                // Continue as long as it's not null terminator, not newline, and not a char we explicitly want to stop at
                // For general strings, allow alphanumeric, punctuation, and spaces.
                while (*value_end && *value_end != '\n' && (isalnum(*value_end) || ispunct(*value_end) || isspace(*value_end))) {
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
                    goto end_parsing; // Found the value, exit both loops
                }
            }
            line_start = line_end + ((line_end != NULL && *line_end == '\n') ? 1 : 0); // Move to start of next line
            if (line_start == output_str + strlen(output_str)) break; // Reached end
        }
        current_key_prefix = va_arg(args, const char*); // Try next key
    }

end_parsing:
    va_end(args);
    return result_str; // Caller must free this memory
}


// New function: Populates DeviceInfo from udevadm output (primary for ID_*)
void populate_device_info_from_udevadm(DeviceInfo* info) {
    char command[MAX_FULL_PATH_LEN + 64];
    snprintf(command, sizeof(command), "udevadm info --query=property --name=%s", info->dev_path);
    
    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        // fprintf(stderr, "Warning: Failed to run udevadm for %s\n", info->dev_path);
        return;
    }

    char line[MAX_BUFFER_LEN];
    while (fgets(line, sizeof(line), fp) != NULL) {
        // Remove trailing newline
        line[strcspn(line, "\n")] = 0;

        // Parse KEY=VALUE pairs
        char* equals_pos = strchr(line, '=');
        if (equals_pos != NULL) {
            *equals_pos = '\0'; // Null-terminate key
            const char* key = line;
            const char* value = equals_pos + 1;

            if (strcmp(key, "ID_BUS") == 0) {
                if (strcmp(value, "ata") == 0) info->bus_type = BUS_TYPE_ATA;
                else if (strcmp(value, "scsi") == 0) info->bus_type = BUS_TYPE_SCSI;
                else if (strcmp(value, "usb") == 0) info->bus_type = BUS_TYPE_USB;
                else if (strcmp(value, "nvme") == 0) info->bus_type = BUS_TYPE_NVME;
                else if (strcmp(value, "mmc") == 0) info->bus_type = BUS_TYPE_MMC;
                else if (strcmp(value, "virtio") == 0) info->bus_type = BUS_TYPE_VIRTIO;
                else info->bus_type = BUS_TYPE_UNKNOWN;
            } else if (strcmp(key, "ID_MODEL") == 0) {
                strncpy(info->model, value, sizeof(info->model)-1);
            } else if (strcmp(key, "ID_VENDOR_FROM_DATABASE") == 0) { // More specific vendor
                strncpy(info->vendor, value, sizeof(info->vendor)-1);
            } else if (strcmp(key, "ID_VENDOR") == 0 && strlen(info->vendor) == 0) { // Fallback vendor
                strncpy(info->vendor, value, sizeof(info->vendor)-1);
            } else if (strcmp(key, "ID_SERIAL") == 0) {
                strncpy(info->serial, value, sizeof(info->serial)-1);
            } else if (strcmp(key, "ID_REVISION") == 0) {
                strncpy(info->firmware_rev, value, sizeof(info->firmware_rev)-1);
            }
            // Note: udevadm often provides ID_TYPE=disk or ID_FS_TYPE for partitions.
            // DeviceType will be determined later using rotational and bus_type
        }
    }
    pclose(fp);
}


// Adjusted function: Populates DeviceInfo from sysfs (for total sectors, block sizes, rotational)
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

    // Determine Device Type based on rotational property and bus type (if already known from udevadm)
    char* rotational_str = NULL;
    snprintf(path_buf, sizeof(path_buf), "%s/queue/rotational", sysfs_base_path);
    rotational_str = read_sysfs_attribute(path_buf);
    
    if (info->bus_type == BUS_TYPE_NVME) {
        info->type = DEVICE_TYPE_NVME_SSD;
    } else if (info->bus_type == BUS_TYPE_USB) {
        info->type = DEVICE_TYPE_USB_STORAGE;
    } else if (rotational_str && strcmp(rotational_str, "0") == 0) {
        info->type = DEVICE_TYPE_SATA_SSD;
    } else if (rotational_str && strcmp(rotational_str, "1") == 0) {
        info->type = DEVICE_TYPE_HDD;
    } else {
        info->type = DEVICE_TYPE_UNKNOWN;
    }

    if (rotational_str) free(rotational_str);

    // If model, vendor, serial, firmware_rev were not obtained by udevadm,
    // (e.g. if udevadm isn't available or doesn't expose them),
    // then try to get them from sysfs as a fallback.
    // This is less common but provides robustness.

    // NVMe devices have different sysfs paths for these details, handle them separately.
    if (info->type == DEVICE_TYPE_NVME_SSD) {
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
        
        if (strlen(info->model) == 0) {
            attr_value = read_sysfs_attribute_then_copy(nvme_class_path, "model", info->model, sizeof(info->model));
            if (attr_value) free(attr_value);
        }
        if (strlen(info->firmware_rev) == 0) {
            attr_value = read_sysfs_attribute_then_copy(nvme_class_path, "firmware_rev", info->firmware_rev, sizeof(info->firmware_rev));
            if (attr_value) free(attr_value);
        }
        if (strlen(info->serial) == 0) {
            attr_value = read_sysfs_attribute_then_copy(nvme_class_path, "serial", info->serial, sizeof(info->serial));
            if (attr_value) free(attr_value);
        }
        if (strlen(info->vendor) == 0) {
            snprintf(path_buf, sizeof(path_buf), "/sys/class/nvme/%s/vendor", nvme_ctrl_name);
            attr_value = read_sysfs_attribute(path_buf);
            if (attr_value) { strncpy(info->vendor, attr_value, sizeof(info->vendor)-1); free(attr_value); }
            else { // Fallback for vendor
                snprintf(path_buf, sizeof(path_buf), "/sys/class/nvme/%s/device/vendor", nvme_ctrl_name);
                attr_value = read_sysfs_attribute(path_buf);
                if (attr_value) { strncpy(info->vendor, attr_value, sizeof(info->vendor)-1); free(attr_value); }
            }
        }
    } else { // SATA/USB/MMC, etc.
        if (strlen(info->model) == 0) {
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


// New function: Populates DeviceInfo from smartctl output string (for RPM and serial fallback)
void populate_device_info_from_smartctl_output(DeviceInfo* info, const char* smartctl_output) {
    // Get RPM if it's an HDD
    if (info->type == DEVICE_TYPE_HDD && strlen(info->rotation_rate) == 0) {
        char* rpm_value = get_string_from_output(smartctl_output, "Rotation Rate:", NULL);
        if (rpm_value) {
            strncpy(info->rotation_rate, rpm_value, sizeof(info->rotation_rate)-1);
            free(rpm_value);
        }
    }

    // Get Serial Number if sysfs or udevadm didn't provide it
    if (strlen(info->serial) == 0) {
        char* serial_value = get_string_from_output(smartctl_output, 
            "Serial Number:",        // Standard
            "Serial number:",        // Lowercase 'n'
            "Serial:",               // Generic fallback
            NULL);
        if (serial_value) {
            strncpy(info->serial, serial_value, sizeof(info->serial)-1);
            free(serial_value);
        }
    }
    // Could also add model/vendor/firmware fallbacks from smartctl if udevadm/sysfs miss them.
    // E.g., info->model if empty, check "Device Model:"
}
