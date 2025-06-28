// disk_collector.c
#include "disk_collector.h"
#include "device_info.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <sys/sysmacros.h>
#include <libgen.h>
#include <ctype.h>
#include <stdarg.h> // Needed for va_list in get_string_from_output


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


// Runs smartctl and captures its entire output
char* run_smartctl(const char* dev_path) {
    char command[MAX_FULL_PATH_LEN + 32]; // smartctl -a /dev/device
    snprintf(command, sizeof(command), "sudo smartctl -a %s", dev_path);

    FILE *fp = popen(command, "r");
    if (fp == NULL) {
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

// Parses a string for multiple potential keys (from smartctl or udevadm output)
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
                // For general strings, allow alphanumeric, punctuation, and spaces.
                while (*value_end && *value_end != '\n' && (isalnum(*value_end) || ispunct(*value_end) || isspace(*value_end) || *value_end == '[' || *value_end == ']')) {
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


// Populates DeviceInfo from udevadm output (primary for ID_*)
void populate_device_info_from_udevadm(DeviceInfo* info) {
    char command[MAX_FULL_PATH_LEN + 64];
    snprintf(command, sizeof(command), "udevadm info --query=property --name=%s", info->dev_path);
    
    FILE *fp = popen(command, "r");
    if (fp == NULL) {
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
            } else if (strcmp(key, "ID_VENDOR_FROM_DATABASE") == 0) { // More specific vendor from udev database
                strncpy(info->vendor, value, sizeof(info->vendor)-1);
            } else if (strcmp(key, "ID_VENDOR") == 0 && strlen(info->vendor) == 0) { // Fallback vendor
                strncpy(info->vendor, value, sizeof(info->vendor)-1);
            } else if (strcmp(key, "ID_SERIAL") == 0) {
                strncpy(info->serial, value, sizeof(info->serial)-1);
            } else if (strcmp(key, "ID_REVISION") == 0) {
                strncpy(info->firmware_rev, value, sizeof(info->firmware_rev)-1);
            } else if (strcmp(key, "DEVPATH") == 0) {
                // Parse DEVPATH for bus type if ID_BUS is not strong enough
                // Example: /devices/pci0000:00/0000:00:17.0/ata2/host1/target1:0:0/1:0:0:0/block/sda
                if (info->bus_type == BUS_TYPE_UNKNOWN) {
                    if (strstr(value, "/ata") != NULL) {
                        info->bus_type = BUS_TYPE_ATA;
                    } else if (strstr(value, "/usb") != NULL || strstr(value, "/host") != NULL) { // host usually means scsi_host for USB drives too
                        // check if the subsystem is "usb" or "scsi" (for USB-SCSI bridge devices)
                        char sysfs_subsystem_path[MAX_FULL_PATH_LEN];
                        snprintf(sysfs_subsystem_path, sizeof(sysfs_subsystem_path), "/sys/block/%s/device/subsystem", info->main_dev_name);
                        char* subsystem_val = read_sysfs_attribute(sysfs_subsystem_path);
                        if (subsystem_val) {
                            if (strcmp(subsystem_val, "usb") == 0) info->bus_type = BUS_TYPE_USB;
                            else if (strcmp(subsystem_val, "scsi") == 0) info->bus_type = BUS_TYPE_SCSI; // Covers USB-SCSI bridges
                            free(subsystem_val);
                        }
                    } else if (strstr(value, "/nvme") != NULL) {
                        info->bus_type = BUS_TYPE_NVME;
                    } else if (strstr(value, "/mmc") != NULL) {
                        info->bus_type = BUS_TYPE_MMC;
                    }
                }
            }
        }
    }
    pclose(fp);

    // Final fallback for ID_BUS if not explicitly found by udevadm properties, try sysfs directly
    if (info->bus_type == BUS_TYPE_UNKNOWN) {
        char sysfs_subsystem_path[MAX_FULL_PATH_LEN];
        snprintf(sysfs_subsystem_path, sizeof(sysfs_subsystem_path), "/sys/block/%s/device/subsystem", info->main_dev_name);
        char* subsystem_value = read_sysfs_attribute(sysfs_subsystem_path);
        if (subsystem_value) {
            if (strcmp(subsystem_value, "ata") == 0) info->bus_type = BUS_TYPE_ATA;
            else if (strcmp(subsystem_value, "scsi") == 0) info->bus_type = BUS_TYPE_SCSI;
            else if (strcmp(subsystem_value, "usb") == 0) info->bus_type = BUS_TYPE_USB;
            free(subsystem_value);
        }
    }
}


// Populates DeviceInfo from sysfs (for total sectors, block sizes, rotational)
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
        // If bus type is ATA/SCSI/UNKNOWN and rotational is 0, it's an SSD
        if (info->bus_type == BUS_TYPE_ATA || info->bus_type == BUS_TYPE_UNKNOWN) {
             info->type = DEVICE_TYPE_SATA_SSD;
        } else { // Could be a SCSI SSD or other unknown SSD type
            info->type = DEVICE_TYPE_UNKNOWN_SSD;
        }
    } else if (rotational_str && strcmp(rotational_str, "1") == 0) {
        info->type = DEVICE_TYPE_HDD;
    } else {
        info->type = DEVICE_TYPE_UNKNOWN;
    }

    if (rotational_str) free(rotational_str);

    // Fallback to sysfs for model, vendor, serial, firmware_rev if not obtained by udevadm
    // This is less common but provides robustness if udevadm fails or doesn't provide these specific IDs.

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


// Populates DeviceInfo from smartctl output string (for RPM and serial fallback, and now vendor/bus type fallback)
void populate_device_info_from_smartctl_output(DeviceInfo* info, const char* smartctl_output) {
    // Get RPM if it's an HDD
    if (info->type == DEVICE_TYPE_HDD && info->rotation_rate_rpm == 0) { // Check if RPM is not yet populated
        char* rpm_str_value = get_string_from_output(smartctl_output, "Rotation Rate:", NULL);
        if (rpm_str_value) {
            // Find the first non-digit character (e.g., ' ') and null-terminate there
            char* endptr;
            for (endptr = rpm_str_value; *endptr != '\0'; ++endptr) {
                if (!isdigit(*endptr)) {
                    *endptr = '\0'; // Truncate at first non-digit
                    break;
                }
            }
            info->rotation_rate_rpm = strtol(rpm_str_value, NULL, 10);
            free(rpm_str_value);
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

    // Always attempt to get Vendor from smartctl, and if successful, overwrite previous
    // This is because smartctl's Model Family is typically the most accurate vendor info.
    char* model_family = get_string_from_output(smartctl_output, "Model Family:", NULL);
    if (model_family) {
        // "Western Digital Ultrastar DC HC550" -> "Western Digital"
        char *space_pos1 = strchr(model_family, ' ');
        if (space_pos1) {
            char *space_pos2 = strchr(space_pos1 + 1, ' ');
            if (space_pos2) { // Found two spaces, try to get "Western Digital"
                size_t vendor_len = space_pos2 - model_family;
                if (vendor_len < sizeof(info->vendor)) {
                    strncpy(info->vendor, model_family, vendor_len);
                    info->vendor[vendor_len] = '\0';
                } else { // Fallback if it's too long, just copy what fits
                    strncpy(info->vendor, model_family, sizeof(info->vendor)-1);
                }
            } else { // Only one space, take the first word as vendor
                size_t vendor_len = space_pos1 - model_family;
                strncpy(info->vendor, model_family, vendor_len);
                info->vendor[vendor_len] = '\0';
            }
        } else { // No spaces, whole thing is the vendor/model family
            strncpy(info->vendor, model_family, sizeof(info->vendor)-1);
        }
        free(model_family);
    } else {
        // Fallback to Device Model if Model Family not found, take the first word as vendor
        char* device_model = get_string_from_output(smartctl_output, "Device Model:", NULL);
        if (device_model) {
            char *space_pos = strchr(device_model, ' ');
            if (space_pos) {
                size_t vendor_len = space_pos - device_model;
                strncpy(info->vendor, device_model, vendor_len);
                info->vendor[vendor_len] = '\0';
            } else {
                strncpy(info->vendor, device_model, sizeof(info->vendor)-1);
            }
            free(device_model);
        }
    }


    // Get Bus Type from smartctl "SATA Version is:" if it's still unknown
    if (info->bus_type == BUS_TYPE_UNKNOWN) {
        char* sata_version_str = get_string_from_output(smartctl_output, "SATA Version is:", NULL);
        if (sata_version_str) {
            if (strstr(sata_version_str, "SATA") != NULL) {
                info->bus_type = BUS_TYPE_ATA; // SATA is part of ATA
            }
            free(sata_version_str);
        }
        // Also check for "ATA Version is:"
        if (info->bus_type == BUS_TYPE_UNKNOWN) {
            char* ata_version_str = get_string_from_output(smartctl_output, "ATA Version is:", NULL);
            if (ata_version_str) {
                if (strstr(ata_version_str, "ATA") != NULL) {
                    info->bus_type = BUS_TYPE_ATA;
                }
                free(ata_version_str);
            }
        }
    }

    // --- New: Populate nominal_capacity_str from smartctl output ---
    // Look for User Capacity (for ATA/SATA devices)
    char* user_capacity_str = get_string_from_output(smartctl_output, "User Capacity:", NULL);
    if (user_capacity_str) {
        // Example: "16,000,900,661,248 bytes [16.0 TB]" -> extract "[16.0 TB]"
        char* bracket_start = strchr(user_capacity_str, '[');
        if (bracket_start) {
            char* bracket_end = strchr(bracket_start, ']');
            if (bracket_end) {
                size_t len = bracket_end - bracket_start + 1; // Include brackets
                if (len < sizeof(info->nominal_capacity_str)) {
                    strncpy(info->nominal_capacity_str, bracket_start, len);
                    info->nominal_capacity_str[len] = '\0';
                } else {
                    strncpy(info->nominal_capacity_str, bracket_start, sizeof(info->nominal_capacity_str) - 1);
                    info->nominal_capacity_str[sizeof(info->nominal_capacity_str) - 1] = '\0';
                }
            }
        }
        free(user_capacity_str);
    } else {
        // If not User Capacity, look for Total NVM Capacity (for NVMe devices)
        char* nvm_capacity_str = get_string_from_output(smartctl_output, "Total NVM Capacity:", NULL);
        if (nvm_capacity_str) {
             // Example: "1,024,209,543,168 [1.02 TB]" -> extract "[1.02 TB]"
            char* bracket_start = strchr(nvm_capacity_str, '[');
            if (bracket_start) {
                char* bracket_end = strchr(bracket_start, ']');
                if (bracket_end) {
                    size_t len = bracket_end - bracket_start + 1; // Include brackets
                     if (len < sizeof(info->nominal_capacity_str)) {
                        strncpy(info->nominal_capacity_str, bracket_start, len);
                        info->nominal_capacity_str[len] = '\0';
                    } else {
                        strncpy(info->nominal_capacity_str, bracket_start, sizeof(info->nominal_capacity_str) - 1);
                        info->nominal_capacity_str[sizeof(info->nominal_capacity_str) - 1] = '\0';
                    }
                }
            }
            free(nvm_capacity_str);
        } else {
            // Fallback for NVMe Namespace Capacity if Total NVM Capacity isn't present
            char* namespace_capacity_str = get_string_from_output(smartctl_output, "Namespace 1 Size/Capacity:", NULL);
            if (namespace_capacity_str) {
                 // Example: "1,024,209,543,168 [1.02 TB]" -> extract "[1.02 TB]"
                char* bracket_start = strchr(namespace_capacity_str, '[');
                if (bracket_start) {
                    char* bracket_end = strchr(bracket_start, ']');
                    if (bracket_end) {
                        size_t len = bracket_end - bracket_start + 1; // Include brackets
                        if (len < sizeof(info->nominal_capacity_str)) {
                            strncpy(info->nominal_capacity_str, bracket_start, len);
                            info->nominal_capacity_str[len] = '\0';
                        } else {
                            strncpy(info->nominal_capacity_str, bracket_start, sizeof(info->nominal_capacity_str) - 1);
                            info->nominal_capacity_str[sizeof(info->nominal_capacity_str) - 1] = '\0';
                        }
                    }
                }
                free(namespace_capacity_str);
            }
        }
    }
}
