// main.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h> // For basename (used by get_main_device_name in disk_collector.c)

#include "device_info.h"    // Contains DeviceInfo struct and print/init functions
#include "disk_collector.h" // Contains data collection functions

// Define PATH_MAX and MAX_BUFFER_LEN here as well, if they are not system-wide
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#define MAX_BUFFER_LEN 128

int main(int argc, const char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "用法: %s <设备路径1> [<设备路径2> ...]\n", argv[0]);
        fprintf(stderr, "示例: %s /dev/nvme0n1 /dev/sda /dev/sdb\n", argv[0]); // Updated example
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        DeviceInfo device;
        init_device_info(&device); // Initialize the structure

        strncpy(device.dev_path, argv[i], sizeof(device.dev_path) - 1);
        device.dev_path[sizeof(device.dev_path) - 1] = '\0';

        char* main_dev = get_main_device_name(device.dev_path);
        if (!main_dev) {
            fprintf(stderr, "错误：无法确定 %s 的主设备名称或它不是块设备。\n", device.dev_path);
            if (i < argc - 1) printf("\n");
            continue; // Skip to next device
        }
        strncpy(device.main_dev_name, main_dev, sizeof(device.main_dev_name) - 1);
        device.main_dev_name[sizeof(device.main_dev_name) - 1] = '\0';
        free(main_dev); // Free memory allocated by get_main_device_name

        // --- Phase 1: Populate info from udevadm (primary for ID_*) ---
        // This is where ID_BUS, ID_MODEL, ID_VENDOR, ID_SERIAL, ID_REVISION come from.
        populate_device_info_from_udevadm(&device);

        // --- Phase 2: Populate info from sysfs (for total sectors, block sizes, rotational, and fallback for ID_*) ---
        // This also determines device.type based on rotational + bus_type, and fills remaining fields from sysfs.
        populate_device_info_from_sysfs(&device);

        // --- Phase 3: Run smartctl once and parse its output for specific fields (RPM, serial fallback) ---
        char* smartctl_full_output = NULL;
        // Only run smartctl if it's an HDD or we need to try getting serial from it
        if (device.type == DEVICE_TYPE_HDD || strlen(device.serial) == 0) {
            smartctl_full_output = run_smartctl(device.dev_path);
            if (smartctl_full_output) {
                populate_device_info_from_smartctl_output(&device, smartctl_full_output);
                free(smartctl_full_output); // Free the large output string
            }
        }
        
        // --- Phase 4: Print all collected info ---
        print_device_info(&device);

        if (i < argc - 1) {
            printf("\n"); // Add a newline between different device info blocks for readability
        }
    }

    return 0;
}
