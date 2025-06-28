// src/device_info/generic_info.h
#ifndef GENERIC_INFO_H
#define GENERIC_INFO_H

#include "device_info.h"
#include <stddef.h>
#include <stdarg.h>

// 缺少的常量定义
#define MAX_SERIAL_LEN 64
#define MAX_VENDOR_LEN 64
#define MAX_MODEL_LEN 128
#define MAX_FW_REV_LEN 32

// 通用设备信息收集函数
int collect_generic_info(DeviceInfo* info);
int collect_sysfs_info(DeviceInfo* info);
int collect_udevadm_info(DeviceInfo* info);

#if USE_SMARTCTL
int collect_smartctl_info(DeviceInfo* info);
#endif

// 辅助函数
char* get_main_device_name(const char* dev_path);
char* run_command_output(const char* command);
int read_sysfs_file(const char* base_path, const char* subpath, char* buffer, size_t buffer_size);
char* trim_whitespace(char* str);

// 通用输出解析函数
typedef enum {
    PARSER_COLON,           // key: value (smartctl, nvme-cli)
    PARSER_EQUALS,          // key=value (udevadm)
    PARSER_SPACE,           // key value (某些命令)
    PARSER_AUTO             // 自动检测分隔符
} ParserType;

// 通用 key/value 提取函数
char* extract_value_from_output(const char* output, char* buffer, size_t buffer_size,
                                ParserType parser_type, const char* first_key, ...);

// 提取括号内容的函数
char* extract_bracketed_value(const char* output, char* buffer, size_t buffer_size,
                              const char* first_key, ...);

// 提取第一个单词的函数
char* extract_first_word(const char* input, char* buffer, size_t buffer_size);

// 行查找和值提取的组合函数
char* find_line_and_extract_value(const char* output, char* buffer, size_t buffer_size,
                                  ParserType parser_type, const char* first_key, ...);

// 兼容旧接口的函数（保持向后兼容）
char* get_string_from_output(const char* output, char* buffer, size_t buffer_size, const char* first_key, ...);
char* get_bracketed_string_from_output(const char* output, char* buffer, size_t buffer_size, const char* first_key, ...);
char* get_first_word(const char* input, char* buffer, size_t buffer_size);

#endif // GENERIC_INFO_H
