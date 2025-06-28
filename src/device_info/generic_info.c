// src/device_info/generic_info.c

#include "generic_info.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <libgen.h>

// 内部辅助函数声明
static char auto_detect_separator(const char* text);
static BusType parse_bus_type_string(const char* bus_str);

// 收集通用设备信息
int collect_generic_info(DeviceInfo* info) {
    int success_count = 0;
    int total_attempts = 0;

    // 获取主设备名
    char* main_dev_name = get_main_device_name(info->dev_path);
    if (main_dev_name) {
        strncpy(info->main_dev_name, main_dev_name, sizeof(info->main_dev_name) - 1);
        info->main_dev_name[sizeof(info->main_dev_name) - 1] = '\0';
        free(main_dev_name);
        success_count++;
    }
    total_attempts++;

    // 从 sysfs 收集基本信息
    total_attempts++;
    if (collect_sysfs_info(info) == 0) {
        success_count++;
    }

    // 从 udevadm 收集信息
    total_attempts++;
    if (collect_udevadm_info(info) == 0) {
        success_count++;
    }

    return (success_count > 0) ? 0 : -1;
}

// 从 sysfs 收集信息
int collect_sysfs_info(DeviceInfo* info) {
    char sysfs_base_path[MAX_FULL_PATH_LEN];
    char buffer[128];

    snprintf(sysfs_base_path, sizeof(sysfs_base_path), "/sys/block/%s", info->main_dev_name);

    if (access(sysfs_base_path, F_OK) != 0) {
        return -1;
    }

    int found_info = 0;

    // 获取总扇区数
    if (read_sysfs_file(sysfs_base_path, "size", buffer, sizeof(buffer)) == 0) {
        info->total_sectors = strtoull(buffer, NULL, 10);
        found_info = 1;
    }

    // 获取逻辑块大小
    if (read_sysfs_file(sysfs_base_path, "queue/logical_block_size", buffer, sizeof(buffer)) == 0) {
        info->logical_block_size = strtoul(buffer, NULL, 10);
    }

    // 获取物理块大小
    if (read_sysfs_file(sysfs_base_path, "queue/physical_block_size", buffer, sizeof(buffer)) == 0) {
        info->physical_block_size = strtoul(buffer, NULL, 10);
    }

    // 获取最优 I/O 大小
    if (read_sysfs_file(sysfs_base_path, "queue/optimal_io_size", buffer, sizeof(buffer)) == 0) {
        int optimal_size = atoi(buffer);
        if (optimal_size > 0) {
            info->optimal_io_size = optimal_size;
        }
    }

    // 计算容量
    if (info->total_sectors > 0 && info->logical_block_size > 0) {
        double total_bytes = (double)info->total_sectors * info->logical_block_size;
        info->capacity_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
    }

    // 检查 rotational 属性
    if (read_sysfs_file(sysfs_base_path, "queue/rotational", buffer, sizeof(buffer)) == 0) {
        int rotational = atoi(buffer);
        info->is_rotational = rotational;
        found_info = 1;
    }

    // 尝试获取设备型号和厂商信息
    if (strlen(info->model) == 0 || strcmp(info->model, "Unknown") == 0) {
        if (read_sysfs_file(sysfs_base_path, "device/model", buffer, sizeof(buffer)) == 0) {
            char* trimmed = trim_whitespace(buffer);
            if (strlen(trimmed) > 0) {
                strncpy(info->model, trimmed, sizeof(info->model) - 1);
                info->model[sizeof(info->model) - 1] = '\0';
                found_info = 1;
            }
        }
    }

    if (strlen(info->vendor) == 0 || strcmp(info->vendor, "Unknown") == 0) {
        if (read_sysfs_file(sysfs_base_path, "device/vendor", buffer, sizeof(buffer)) == 0) {
            char* trimmed = trim_whitespace(buffer);
            if (strlen(trimmed) > 0) {
                strncpy(info->vendor, trimmed, sizeof(info->vendor) - 1);
                info->vendor[sizeof(info->vendor) - 1] = '\0';
                found_info = 1;
            }
        }
    }

    return found_info ? 0 : -1;
}

// 从 udevadm 收集信息（使用通用解析函数）
int collect_udevadm_info(DeviceInfo* info) {
    char command[MAX_FULL_PATH_LEN + 64];
    snprintf(command, sizeof(command), "udevadm info --query=property --name=%s 2>/dev/null", info->dev_path);

    char* output = run_command_output(command);
    if (!output) {
        return -1;
    }

    char buffer[256];
    int found_info = 0;

    // 解析总线类型 - 支持多种 key，按优先级尝试
    if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_EQUALS,
                                 "SYNO_DEV_DISKPORTTYPE", "ID_BUS", "PHYSDEVBUS", NULL)) {
        BusType detected_bus = parse_bus_type_string(buffer);
        if (detected_bus != BUS_TYPE_UNKNOWN) {
            info->bus_type = detected_bus;
            found_info++;
        }
    }

    // 提取设备信息 - 型号
    if (strlen(info->model) == 0 || strcmp(info->model, "Unknown") == 0) {
        if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_EQUALS,
                                     "ID_MODEL", "ID_MODEL_ENC", NULL)) {
            strncpy(info->model, buffer, sizeof(info->model) - 1);
            info->model[sizeof(info->model) - 1] = '\0';
            found_info++;
        }
    }

    // 提取厂商信息
    if (strlen(info->vendor) == 0 || strcmp(info->vendor, "Unknown") == 0) {
        if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_EQUALS,
                                     "ID_VENDOR", "ID_VENDOR_ENC", NULL)) {
            strncpy(info->vendor, buffer, sizeof(info->vendor) - 1);
            info->vendor[sizeof(info->vendor) - 1] = '\0';
            found_info++;
        }
    }

    // 提取序列号 - 支持多种格式
    if (strlen(info->serial) == 0) {
        if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_EQUALS,
                                     "ID_SERIAL_SHORT", "ID_SERIAL", NULL)) {
            strncpy(info->serial, buffer, sizeof(info->serial) - 1);
            info->serial[sizeof(info->serial) - 1] = '\0';
            found_info++;
        }
    }

    // 提取固件版本
    if (strlen(info->firmware_rev) == 0) {
        if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_EQUALS,
                                     "ID_REVISION", "ID_FW_REVISION", NULL)) {
            strncpy(info->firmware_rev, buffer, sizeof(info->firmware_rev) - 1);
            info->firmware_rev[sizeof(info->firmware_rev) - 1] = '\0';
            found_info++;
        }
    }

    // 检测子系统
    if (extract_value_from_output(output, buffer, sizeof(buffer), PARSER_EQUALS,
                                 "SUBSYSTEM", NULL)) {
        if (strcmp(buffer, "nvme") == 0) {
            info->bus_type = BUS_TYPE_NVME;
            info->device_type = DEVICE_TYPE_NVME_SSD;
            info->is_rotational = 0;
            found_info++;
        }
    }

    free(output);
    return found_info > 0 ? 0 : -1;
}

// 辅助函数：获取主设备名
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

// 运行命令并返回输出
char* run_command_output(const char* command) {
    FILE *fp = popen(command, "r");
    if (fp == NULL) {
        return NULL;
    }

    char* full_output = NULL;
    size_t current_size = 0;
    char buffer[1024];

    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        size_t buffer_len = strlen(buffer);
        full_output = realloc(full_output, current_size + buffer_len + 1);
        if (full_output == NULL) {
            pclose(fp);
            return NULL;
        }
        strcpy(full_output + current_size, buffer);
        current_size += buffer_len;
    }

    pclose(fp);
    return full_output;
}

// 读取 sysfs 文件
int read_sysfs_file(const char* base_path, const char* subpath, char* buffer, size_t buffer_size) {
    char full_path[MAX_FULL_PATH_LEN + 64];
    snprintf(full_path, sizeof(full_path), "%s/%s", base_path, subpath);

    FILE* fp = fopen(full_path, "r");
    if (!fp) {
        return -1;
    }

    if (fgets(buffer, buffer_size, fp) != NULL) {
        fclose(fp);
        buffer[strcspn(buffer, "\n\r")] = '\0';
        return 0;
    }

    fclose(fp);
    return -1;
}

// 去除字符串前后空格
char* trim_whitespace(char* str) {
    char* start = str;
    while (isspace(*start)) start++;

    char* end = start + strlen(start) - 1;
    while (end > start && isspace(*end)) *end-- = '\0';

    return start;
}

/**
 * 通用的 key/value 提取函数
 */
char* extract_value_from_output(const char* output, char* buffer, size_t buffer_size,
                                ParserType parser_type, const char* first_key, ...) {
    if (!output || !buffer || !first_key || buffer_size == 0) {
        return NULL;
    }

    va_list args;
    const char* key = first_key;

    va_start(args, first_key);

    while (key != NULL) {
        char* line = strstr(output, key);
        if (line) {
            char separator;

            switch (parser_type) {
                case PARSER_COLON:
                    separator = ':';
                    break;
                case PARSER_EQUALS:
                    separator = '=';
                    break;
                case PARSER_SPACE:
                    separator = ' ';
                    break;
                case PARSER_AUTO:
                    separator = auto_detect_separator(line + strlen(key));
                    break;
                default:
                    separator = ':';
                    break;
            }

            char* sep_pos = strchr(line, separator);
            if (sep_pos) {
                char* value_start = sep_pos + 1;
                while (*value_start && isspace(*value_start)) {
                    value_start++;
                }

                char* value_end = value_start;
                while (*value_end && *value_end != '\n' && *value_end != '\r') {
                    value_end++;
                }

                while (value_end > value_start && isspace(*(value_end - 1))) {
                    value_end--;
                }

                size_t value_len = value_end - value_start;
                if (value_len > 0 && value_len < buffer_size) {
                    strncpy(buffer, value_start, value_len);
                    buffer[value_len] = '\0';
                    va_end(args);
                    return buffer;
                }
            }
        }

        key = va_arg(args, const char*);
    }

    va_end(args);
    return NULL;
}

/**
 * 自动检测分隔符
 */
static char auto_detect_separator(const char* text) {
    if (!text) return ':';

    while (*text && isspace(*text)) text++;

    for (const char* p = text; *p && *p != '\n' && *p != '\r'; p++) {
        if (*p == ':' || *p == '=' || *p == ' ') {
            return *p;
        }
    }

    return ':';
}

/**
 * 提取括号内的值
 */
char* extract_bracketed_value(const char* output, char* buffer, size_t buffer_size,
                              const char* first_key, ...) {
    if (!output || !buffer || !first_key || buffer_size == 0) {
        return NULL;
    }

    va_list args;
    const char* key = first_key;

    va_start(args, first_key);

    while (key != NULL) {
        char* line = strstr(output, key);
        if (line) {
            char* bracket_start = strchr(line, '[');
            if (bracket_start) {
                char* bracket_end = strchr(bracket_start, ']');
                if (bracket_end && bracket_end > bracket_start + 1) {
                    size_t content_len = bracket_end - bracket_start - 1;
                    if (content_len < buffer_size) {
                        strncpy(buffer, bracket_start + 1, content_len);
                        buffer[content_len] = '\0';
                        va_end(args);
                        return buffer;
                    }
                }
            }
        }

        key = va_arg(args, const char*);
    }

    va_end(args);
    return NULL;
}

/**
 * 提取字符串的第一个单词
 */
char* extract_first_word(const char* input, char* buffer, size_t buffer_size) {
    if (!input || !buffer || buffer_size == 0) {
        return NULL;
    }

    const char* start = input;
    while (*start && isspace(*start)) {
        start++;
    }

    const char* end = start;
    while (*end && !isspace(*end) && *end != '\n' && *end != '\r') {
        end++;
    }

    size_t word_len = end - start;
    if (word_len > 0 && word_len < buffer_size) {
        strncpy(buffer, start, word_len);
        buffer[word_len] = '\0';
        return buffer;
    }

    return NULL;
}

/**
 * 查找包含指定 key 的行，并提取该行的值
 */
char* find_line_and_extract_value(const char* output, char* buffer, size_t buffer_size,
                                  ParserType parser_type, const char* first_key, ...) {
    if (!output || !buffer || !first_key || buffer_size == 0) {
        return NULL;
    }

    va_list args;
    const char* key = first_key;

    va_start(args, first_key);

    while (key != NULL) {
        const char* line_start = output;

        while (line_start && *line_start) {
            const char* line_end = strchr(line_start, '\n');
            if (!line_end) {
                line_end = line_start + strlen(line_start);
            }

            const char* content_start = line_start;
            while (content_start < line_end && isspace(*content_start)) {
                content_start++;
            }

            size_t key_len = strlen(key);
            if (content_start + key_len <= line_end &&
                strncmp(content_start, key, key_len) == 0) {

                size_t line_len = line_end - line_start;
                char* line_copy = malloc(line_len + 1);
                if (line_copy) {
                    strncpy(line_copy, line_start, line_len);
                    line_copy[line_len] = '\0';

                    char* result = extract_value_from_output(line_copy, buffer, buffer_size,
                                                           parser_type, key, NULL);
                    free(line_copy);

                    if (result) {
                        va_end(args);
                        return result;
                    }
                }
            }

            if (*line_end == '\n') {
                line_start = line_end + 1;
            } else {
                break;
            }
        }

        key = va_arg(args, const char*);
    }

    va_end(args);
    return NULL;
}

/**
 * 解析总线类型字符串
 */
static BusType parse_bus_type_string(const char* bus_str) {
    if (!bus_str) return BUS_TYPE_UNKNOWN;

    if (strcasecmp(bus_str, "SATA") == 0) return BUS_TYPE_SATA;
    if (strcasecmp(bus_str, "ata") == 0) return BUS_TYPE_ATA;
    if (strcasecmp(bus_str, "nvme") == 0) return BUS_TYPE_NVME;
    if (strcasecmp(bus_str, "scsi") == 0) return BUS_TYPE_SCSI;
    if (strcasecmp(bus_str, "usb") == 0) return BUS_TYPE_USB;
    if (strcasecmp(bus_str, "mmc") == 0) return BUS_TYPE_MMC;

    return BUS_TYPE_UNKNOWN;
}

#if USE_SMARTCTL
// smartctl 信息收集
int collect_smartctl_info(DeviceInfo* info) {
    char command[MAX_FULL_PATH_LEN + 32];
    snprintf(command, sizeof(command), "sudo smartctl -a %s 2>/dev/null", info->dev_path);

    char* smartctl_output = run_command_output(command);
    if (!smartctl_output) {
        return -1;
    }

    char buffer[256];
    int found_info = 0;

    // 提取转速信息（仅用于HDD）
    if (info->is_rotational == 1 && info->rotation_rate_rpm == 0) {
        if (extract_value_from_output(smartctl_output, buffer, sizeof(buffer), PARSER_COLON, "Rotation Rate", NULL)) {
            if (isdigit(buffer[0])) {
                info->rotation_rate_rpm = strtol(buffer, NULL, 10);
                found_info = 1;
            }
        }
    }

    // 提取序列号
    if (strlen(info->serial) == 0) {
        if (extract_value_from_output(smartctl_output, buffer, sizeof(buffer), PARSER_COLON,
                                  "Serial Number", "Serial number", NULL)) {
            char serial_buffer[MAX_SERIAL_LEN];
            if (extract_first_word(buffer, serial_buffer, sizeof(serial_buffer))) {
                strcpy(info->serial, serial_buffer);
                found_info = 1;
            }
        }
    }

    // 提取型号信息
    if (strlen(info->model) == 0 || strcmp(info->model, "Unknown") == 0) {
        if (extract_value_from_output(smartctl_output, buffer, sizeof(buffer), PARSER_COLON,
                                  "Device Model", "Model Number", "Product", NULL)) {
            if (strlen(buffer) < sizeof(info->model)) {
                strcpy(info->model, buffer);
                found_info = 1;
            }
        }
    }

    // 提取厂商信息
    if (strlen(info->vendor) == 0 || strcmp(info->vendor, "Unknown") == 0) {
        if (extract_value_from_output(smartctl_output, buffer, sizeof(buffer), PARSER_COLON,
                                  "Model Family", "Vendor", NULL)) {
            char vendor_buffer[MAX_VENDOR_LEN];
            if (extract_first_word(buffer, vendor_buffer, sizeof(vendor_buffer))) {
                strcpy(info->vendor, vendor_buffer);
                found_info = 1;
            }
        }
    }

    // 提取固件版本信息
    if (strlen(info->firmware_rev) == 0) {
        if (extract_value_from_output(smartctl_output, buffer, sizeof(buffer), PARSER_COLON,
                                  "Firmware Version", "Revision", "FW Revision", NULL)) {
            if (strlen(buffer) < sizeof(info->firmware_rev)) {
                strcpy(info->firmware_rev, buffer);
                found_info = 1;
            }
        }
    }

    // 提取容量信息
    if (strlen(info->nominal_capacity_str) == 0) {
        if (extract_bracketed_value(smartctl_output, buffer, sizeof(buffer),
                                           "User Capacity", "Total NVM Capacity", NULL)) {
            if (strlen(buffer) < sizeof(info->nominal_capacity_str)) {
                strcpy(info->nominal_capacity_str, buffer);
                found_info = 1;
            }
        }
    }

    free(smartctl_output);
    return found_info ? 0 : -1;
}
#endif

// 兼容旧接口的函数（简单包装新函数）
char* get_string_from_output(const char* output, char* buffer, size_t buffer_size, const char* first_key, ...) {
    va_list args;
    va_start(args, first_key);

    // 收集所有 key 参数
    const char* keys[10] = {0};  // 最多支持10个key
    keys[0] = first_key;
    int key_count = 1;

    const char* key;
    while ((key = va_arg(args, const char*)) != NULL && key_count < 10) {
        keys[key_count++] = key;
    }
    va_end(args);

    // 调用新的通用函数
    switch (key_count) {
        case 1: return extract_value_from_output(output, buffer, buffer_size, PARSER_COLON, keys[0], NULL);
        case 2: return extract_value_from_output(output, buffer, buffer_size, PARSER_COLON, keys[0], keys[1], NULL);
        case 3: return extract_value_from_output(output, buffer, buffer_size, PARSER_COLON, keys[0], keys[1], keys[2], NULL);
        case 4: return extract_value_from_output(output, buffer, buffer_size, PARSER_COLON, keys[0], keys[1], keys[2], keys[3], NULL);
        default: return extract_value_from_output(output, buffer, buffer_size, PARSER_COLON, keys[0], keys[1], keys[2], keys[3], NULL);
    }
}

char* get_bracketed_string_from_output(const char* output, char* buffer, size_t buffer_size, const char* first_key, ...) {
    va_list args;
    va_start(args, first_key);

    const char* keys[10] = {0};
    keys[0] = first_key;
    int key_count = 1;

    const char* key;
    while ((key = va_arg(args, const char*)) != NULL && key_count < 10) {
        keys[key_count++] = key;
    }
    va_end(args);

    switch (key_count) {
        case 1: return extract_bracketed_value(output, buffer, buffer_size, keys[0], NULL);
        case 2: return extract_bracketed_value(output, buffer, buffer_size, keys[0], keys[1], NULL);
        case 3: return extract_bracketed_value(output, buffer, buffer_size, keys[0], keys[1], keys[2], NULL);
        case 4: return extract_bracketed_value(output, buffer, buffer_size, keys[0], keys[1], keys[2], keys[3], NULL);
        default: return extract_bracketed_value(output, buffer, buffer_size, keys[0], keys[1], keys[2], keys[3], NULL);
    }
}

char* get_first_word(const char* input, char* buffer, size_t buffer_size) {
    return extract_first_word(input, buffer, buffer_size);
}
