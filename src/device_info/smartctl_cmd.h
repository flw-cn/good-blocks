// src/device_info/smartctl_cmd.h - 基于 smartctl 命令行的实现
#ifndef SMARTCTL_CMD_H
#define SMARTCTL_CMD_H

#include "device_info.h"

#if USE_SYSTEM_COMMANDS && USE_SMARTCTL

// 基于 smartctl 命令的信息收集
int collect_smartctl_info_cmd(DeviceInfo* info);
void populate_device_info_from_smartctl_output(DeviceInfo* info, const char* smartctl_output);

#endif

#endif // SMARTCTL_CMD_H
