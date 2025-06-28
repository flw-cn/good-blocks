// disk_collector.h
#ifndef DISK_COLLECTOR_H
#define DISK_COLLECTOR_H

#include "device_info.h"
#include <stdarg.h> // For va_list in get_string_from_output

// Define MAX_FULL_PATH_LEN for internal use within disk_collector.c
// It's generally good practice to define this larger than PATH_MAX if you're constructing paths
// especially if they involve multiple concatenated components or command strings.
#ifndef MAX_FULL_PATH_LEN
#define MAX_FULL_PATH_LEN 4096 // A common safe size for full paths and command buffers
#endif

// Function prototypes
char* get_main_device_name(const char* dev_path);

// New: Run smartctl and capture its full output
char* run_smartctl(const char* dev_path);

// New: Parse a string for multiple potential keys
char* get_string_from_output(const char* output_str, const char* key_prefix_format, ...);

// New: Populate info from udevadm output (primary for ID_*)
void populate_device_info_from_udevadm(DeviceInfo* info);

// Adjusted: Populate info from sysfs (for total sectors, block sizes, rotational)
void populate_device_info_from_sysfs(DeviceInfo* info);

// Adjusted: Populate info from smartctl output (for RPM, serial fallback)
void populate_device_info_from_smartctl_output(DeviceInfo* info, const char* smartctl_output);

#endif // DISK_COLLECTOR_H
