// src/main.c - 程序主入口

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#include "device_info/device_info.h"
#include "scanner.h"
#include "scan_options.h"
#include "time_categories.h"

// 程序信息
#define PROGRAM_NAME "good-blocks"
#define PROGRAM_VERSION "2.0.0"
#define PROGRAM_DESCRIPTION "磁盘健康扫描工具"

// 内部函数声明
static void print_program_banner(void);
static void print_system_info(void);
static int check_permissions(const char* device_path);
static int validate_device(const char* device_path);
static void print_compilation_info(void);

/**
 * 打印程序横幅
 */
static void print_program_banner(void) {
    printf("\033[1;36m");
    printf("╔══════════════════════════════════════════════════════════════╗\n");
    printf("║                                                              ║\n");
    printf("║                    %s v%s                        ║\n", PROGRAM_NAME, PROGRAM_VERSION);
    printf("║                    %s                          ║\n", PROGRAM_DESCRIPTION);
    printf("║                                                              ║\n");
    printf("║              专业的磁盘坏块检测和性能评估工具                ║\n");
    printf("║                                                              ║\n");
    printf("╚══════════════════════════════════════════════════════════════╝\n");
    printf("\033[0m\n");
}

/**
 * 打印系统信息
 */
static void print_system_info(void) {
    printf("\033[32m【系统信息】\033[0m\n");

    // 打印当前用户
    printf("运行用户: ");
    if (getuid() == 0) {
        printf("\033[1;31mroot\033[0m (管理员权限)\n");
    } else {
        printf("\033[33m%s\033[0m (普通用户，某些功能可能受限)\n", getenv("USER"));
    }

    // 打印当前时间
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("扫描时间: %s\n", timestamp);

    // 打印工作目录
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("工作目录: %s\n", cwd);
    }

    printf("\n");
}

/**
 * 打印编译信息
 */
static void print_compilation_info(void) {
    printf("\033[32m【编译配置】\033[0m\n");

#if USE_SYSTEM_COMMANDS
    printf("命令模式: 系统命令\n");

#if USE_SMARTCTL
    printf("SMARTCTL: 启用\n");
#else
    printf("SMARTCTL: 禁用\n");
#endif

#if USE_NVME_CLI
    printf("NVMe CLI: 启用\n");
#else
    printf("NVMe CLI: 禁用\n");
#endif

#else
    printf("命令模式: C API\n");
    printf("注意: C API 模式需要相应的开发库\n");
#endif

    printf("编译时间: %s %s\n", __DATE__, __TIME__);

#ifdef __GNUC__
    printf("编译器: GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#endif

    printf("\n");
}

/**
 * 检查设备访问权限
 */
static int check_permissions(const char* device_path) {
    if (access(device_path, R_OK) != 0) {
        fprintf(stderr, "错误: 无法读取设备 %s: %s\n", device_path, strerror(errno));
        if (errno == EACCES) {
            fprintf(stderr, "提示: 尝试以管理员权限运行: sudo %s ...\n", PROGRAM_NAME);
        }
        return -1;
    }

    return 0;
}

/**
 * 验证设备有效性
 */
static int validate_device(const char* device_path) {
    struct stat st;

    if (stat(device_path, &st) != 0) {
        fprintf(stderr, "错误: 无法访问设备 %s: %s\n", device_path, strerror(errno));
        return -1;
    }

    if (!S_ISBLK(st.st_mode)) {
        fprintf(stderr, "错误: %s 不是块设备\n", device_path);
        return -1;
    }

    printf("\033[32m【设备验证】\033[m设备 %s 验证通过\n", device_path);
    return 0;
}

/**
 * 程序主函数
 */
int main(int argc, char* argv[]) {
    // 打印程序横幅
    print_program_banner();

    // 打印系统信息
    print_system_info();

    // 打印编译信息
    print_compilation_info();

    // 解析命令行参数
    ScanOptions opts;
    if (parse_arguments(argc, argv, &opts) != 0) {
        return EXIT_FAILURE;
    }

    // 验证设备
    if (validate_device(opts.device) != 0) {
        return EXIT_FAILURE;
    }

    // 检查权限
    if (check_permissions(opts.device) != 0) {
        return EXIT_FAILURE;
    }

    // 进行设备信息预检查
    printf("\033[1;33m【预检查】\033[m正在进行设备信息预检查...\n");
    DeviceInfo device_info;
    initialize_device_info(&device_info, opts.device);

    int info_result = collect_device_info(&device_info);
    if (info_result != 0) {
        printf("\033[33m【警告】\033[m设备信息收集不完整，但继续进行扫描\n");
        printf("这可能影响自动参数调整功能\n\n");
    } else {
        printf("\033[32m【预检查】\033[m设备信息收集完成\n\n");
    }

    // 显示设备基本信息
    printf("\033[1;34m【设备概览】\033[0m\n");
    printf("设备路径: %s\n", device_info.dev_path);
    printf("设备名称: %s\n", device_info.main_dev_name);
    printf("设备类型: %s\n", get_device_type_str(&device_info));  // 传递指针
    printf("接口类型: %s\n", get_bus_type_str(device_info.bus_type));

    if (strlen(device_info.model) > 0 && strcmp(device_info.model, "Unknown") != 0) {
        printf("设备型号: %s\n", device_info.model);
    }

    if (strlen(device_info.vendor) > 0 && strcmp(device_info.vendor, "Unknown") != 0) {
        printf("厂商信息: %s\n", device_info.vendor);
    }

    if (device_info.capacity_gb > 0) {
        printf("设备容量: %.2f GB\n", device_info.capacity_gb);
    }

    if (is_ssd_device(&device_info)) {
        printf("设备特性: 固态存储设备\n");
    } else if (is_hdd_device(&device_info)) {
        printf("设备特性: 机械硬盘");
        if (device_info.rotation_rate_rpm > 0) {  // 修正字段名
            printf(" (%d RPM)", device_info.rotation_rate_rpm);
        }
        printf("\n");
    }

    printf("\n");

    // 给用户确认机会
    if (isatty(STDIN_FILENO)) {  // 只在交互模式下询问
        printf("\033[1;33m是否继续扫描? [Y/n]: \033[0m");
        fflush(stdout);

        char response[8];
        if (fgets(response, sizeof(response), stdin)) {
            if (response[0] == 'n' || response[0] == 'N') {
                printf("扫描已取消\n");
                return EXIT_SUCCESS;
            }
        }
        printf("\n");
    }

    // 执行扫描
    printf("\033[1;32m【开始扫描】\033[m启动磁盘健康扫描程序...\n\n");

    int scan_result = scan_device(&opts);

    // 打印使用建议
    printf("\n\033[1;36m【使用建议】\033[0m\n");

    if (scan_result == 0) {         // 扫描正常结束
        printf("1. 查看扫描报告了解设备健康状况\n");
        printf("2. 如果发现性能问题，建议:\n");
        printf("   - 对问题区域进行更详细的扫描\n");
        printf("   - 检查设备 SMART 状态\n");
        printf("   - 考虑数据备份\n");

        if (is_hdd_device(&device_info)) {
            printf("3. 机械硬盘建议定期进行健康扫描\n");
            printf("4. 发现坏块时及时进行数据迁移\n");
        } else if (is_ssd_device(&device_info)) {
            printf("3. SSD 建议关注写入寿命和性能下降趋势\n");
            printf("4. 避免频繁的全盘扫描以延长 SSD 寿命\n");
        }
    } else if (scan_result != 1) {  // 扫描异常中断（1 为用户主动中断）
        printf("1. 扫描未完成，建议稍后重试\n");
        printf("2. 如果持续失败，检查:\n");
        printf("   - 设备是否正常连接\n");
        printf("   - 是否有足够的系统权限\n");
        printf("   - 设备是否正在被其他程序使用\n");
    }

    if (opts.log_filename) {
        printf("\n详细扫描日志已保存至: %s\n", opts.log_filename);
    }

    printf("\n感谢使用 %s！\n", PROGRAM_NAME);

    return scan_result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
