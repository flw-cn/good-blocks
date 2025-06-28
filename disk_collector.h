// disk_collector.h
#ifndef DISK_COLLECTOR_H
#define DISK_COLLECTOR_H

#include "device_info.h"

// Function prototypes
char* get_main_device_name(const char* dev_path);
void populate_device_info_from_sysfs(DeviceInfo* info);
void populate_device_info_from_smartctl(DeviceInfo* info); // Now also gets serial

#endif // DISK_COLLECTOR_Hendif // DISK_COLLECTOR_H
