// src/device_info/nvme_info.h
#ifndef NVME_INFO_H
#define NVME_INFO_H

#include "device_info.h"

// NVMe 设备信息收集
int collect_nvme_info(DeviceInfo* info);

#if USE_SYSTEM_COMMANDS
int collect_nvme_info_cmd(DeviceInfo* info);
#if USE_NVME_CLI
int collect_nvme_cli_info(DeviceInfo* info);
#endif
#endif

#endif // NVME_INFO_H
