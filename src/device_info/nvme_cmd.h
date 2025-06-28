// src/device_info/nvme_cmd.h - 基于 nvme-cli 命令行的实现
#ifndef NVME_CMD_H
#define NVME_CMD_H

#include "device_info.h"

#if USE_SYSTEM_COMMANDS && USE_NVME_CLI

// 基于 nvme-cli 命令的信息收集
int collect_nvme_cli_info_cmd(DeviceInfo* info);

#endif

#endif // NVME_CMD_H
