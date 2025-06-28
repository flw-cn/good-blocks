// src/config.h - 项目配置和编译选项
#ifndef CONFIG_H
#define CONFIG_H

// 项目版本信息
#define PROJECT_VERSION_MAJOR 2
#define PROJECT_VERSION_MINOR 0
#define PROJECT_VERSION_PATCH 0
#define PROJECT_VERSION_STRING "2.0.0"

// 编译时配置（通过 Makefile 传入）
#ifndef USE_SYSTEM_COMMANDS
#define USE_SYSTEM_COMMANDS 1
#endif

#ifndef USE_SMARTCTL
#define USE_SMARTCTL 1
#endif

#ifndef USE_NVME_CLI
#define USE_NVME_CLI 1
#endif

// 调试配置
#ifdef DEBUG
#define DEBUG_PRINT(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...) do {} while(0)
#endif

// 平台特定配置
#ifdef __linux__
#define PLATFORM_LINUX 1
#else
#define PLATFORM_LINUX 0
#endif

// 功能开关
#define ENABLE_COLOR_OUTPUT 1
#define ENABLE_PROGRESS_BAR 1
#define ENABLE_SIGNAL_HANDLING 1

// 性能配置
#define DEFAULT_BUFFER_SIZE 4096
#define MAX_CONCURRENT_READS 1
#define DEFAULT_TIMEOUT_MS 30000

// 文件路径限制
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#endif // CONFIG_H
