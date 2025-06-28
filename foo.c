// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "device_info.h"
#include "disk_collector.h" // Assuming this contains get_device_info


// initialize_device_info is now in device_info.c, no need to include here.
// print_device_info is now in device_info.c, no need to include here.


int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <设备路径>\n", argv[0]);
        return 1;
    }

    const char* dev_path = argv[1];
    DeviceInfo info;
    initialize_device_info(&info, dev_path); // initialize_device_info is still called

    // Get main device name (e.g., sda from /dev/sda1)
    char* main_dev = get_main_device_name(dev_path);
    if (main_dev) {
        strncpy(info.main_dev_name, main_dev, sizeof(info.main_dev_name) - 1);
        info.main_dev_name[sizeof(info.main_dev_name) - 1] = '\0';
        free(main_dev);
    } else {
        fprintf(stderr, "错误: 无法获取主设备名 (%s).\n", dev_path);
        return 1;
    }

    // Populate info from udevadm
    populate_device_info_from_udevadm(&info);

    // Populate info from sysfs
    populate_device_info_from_sysfs(&info);

    // Run smartctl and populate info from its output
    char* smartctl_output = run_smartctl(dev_path);
    if (smartctl_output) {
        populate_device_info_from_smartctl_output(&info, smartctl_output);
        free(smartctl_output);
    } else {
        fprintf(stderr, "警告: 无法运行 smartctl 或获取其输出。部分信息可能缺失。\n");
    }
    
    print_device_info(&info); // print_device_info is still called

    return 0;
}
