// disk_collector.h
#ifndef DISK_COLLECTOR_H
#define DISK_COLLECTOR_H

#include "device_info.h"
#include <stdarg.h> // For va_list in get_string_from_output

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

#endif // DISK_COLLECTOR_Hendif // DISK_COLLECTOR_Hendif // DISK_COLLECTOR_H
