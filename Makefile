# Makefile for good-blocks disk health scanner
# 磁盘健康扫描工具构建配置

# 项目配置
PROJECT_NAME = good-blocks
VERSION = 2.0.0

# 目录配置
SRC_DIR = src
BUILD_DIR = build
DEVICE_INFO_DIR = $(SRC_DIR)/device_info

# 编译器和基本标志
CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c99 -D_GNU_SOURCE
LDFLAGS = -lm -lpthread

# 包含路径
CFLAGS += -I$(SRC_DIR) -I$(DEVICE_INFO_DIR)

# 编译选项配置
USE_SYSTEM_COMMANDS ?= 1    # 1=使用系统命令, 0=使用C API
USE_SMARTCTL ?= 1           # 是否启用 smartctl 支持
USE_NVME_CLI ?= 1           # 是否启用 nvme-cli 支持

# 根据编译选项设置宏定义
ifeq ($(USE_SYSTEM_COMMANDS),1)
    CFLAGS += -DUSE_SYSTEM_COMMANDS=1
    $(info 编译配置: 使用系统命令)
else
    CFLAGS += -DUSE_SYSTEM_COMMANDS=0
    $(info 编译配置: 使用 C API)
    # 如果使用 C API，需要链接相应的库
    # LDFLAGS += -lnvme -latasmart -ludev
endif

ifeq ($(USE_SMARTCTL),1)
    CFLAGS += -DUSE_SMARTCTL=1
    $(info 编译配置: 启用 smartctl 支持)
else
    CFLAGS += -DUSE_SMARTCTL=0
    $(info 编译配置: 禁用 smartctl 支持)
endif

ifeq ($(USE_NVME_CLI),1)
    CFLAGS += -DUSE_NVME_CLI=1
    $(info 编译配置: 启用 nvme-cli 支持)
else
    CFLAGS += -DUSE_NVME_CLI=0
    $(info 编译配置: 禁用 nvme-cli 支持)
endif

# 源文件
MAIN_SRCS = $(SRC_DIR)/main.c \
            $(SRC_DIR)/scanner.c \
            $(SRC_DIR)/scan_options.c \
            $(SRC_DIR)/time_categories.c \
            $(SRC_DIR)/retest.c

DEVICE_INFO_SRCS = $(DEVICE_INFO_DIR)/device_info.c \
                   $(DEVICE_INFO_DIR)/generic_info.c \
                   $(DEVICE_INFO_DIR)/sata_info.c \
                   $(DEVICE_INFO_DIR)/nvme_info.c \
                   $(DEVICE_INFO_DIR)/usb_info.c

ALL_SRCS = $(MAIN_SRCS) $(DEVICE_INFO_SRCS)

# 目标文件
OBJS = $(ALL_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# 主要目标
TARGET = $(BUILD_DIR)/$(PROJECT_NAME)

# 默认目标
.PHONY: all clean install uninstall system-cmd-version static-version debug release dirs config help

all: config dirs $(TARGET)

# 创建目录
dirs:
	@echo "创建构建目录..."
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/device_info

# 主程序目标
$(TARGET): $(OBJS)
	@echo "链接程序: $(TARGET)"
	$(CC) $(OBJS) -o $@ $(LDFLAGS)
	@echo "构建完成: $(TARGET)"

# 编译规则
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "编译: $<"
	$(CC) $(CFLAGS) -c $< -o $@

# 预定义的构建变体
system-cmd-version:
	@echo "构建系统命令版本..."
	$(MAKE) USE_SYSTEM_COMMANDS=1 USE_SMARTCTL=1 USE_NVME_CLI=1 all

static-version:
	@echo "构建静态 API 版本..."
	$(MAKE) USE_SYSTEM_COMMANDS=0 USE_SMARTCTL=0 USE_NVME_CLI=0 all

debug:
	@echo "构建调试版本..."
	$(MAKE) CFLAGS="$(CFLAGS) -g -DDEBUG -O0" all

release:
	@echo "构建发布版本..."
	$(MAKE) CFLAGS="$(CFLAGS) -O3 -DNDEBUG -s" all

# 安装
install: $(TARGET)
	@echo "安装程序到系统..."
	@if [ "$$(id -u)" != "0" ]; then \
		echo "错误: 需要管理员权限进行安装"; \
		echo "请使用: sudo make install"; \
		exit 1; \
	fi
	install -m 755 $(TARGET) /usr/local/bin/$(PROJECT_NAME)
	@echo "安装完成: /usr/local/bin/$(PROJECT_NAME)"

# 卸载
uninstall:
	@echo "卸载程序..."
	@if [ "$$(id -u)" != "0" ]; then \
		echo "错误: 需要管理员权限进行卸载"; \
		echo "请使用: sudo make uninstall"; \
		exit 1; \
	fi
	rm -f /usr/local/bin/$(PROJECT_NAME)
	@echo "卸载完成"

# 清理
clean:
	@echo "清理构建文件..."
	rm -rf $(BUILD_DIR)
	@echo "清理完成"

# 显示当前配置
config:
	@echo "==============================================="
	@echo "           $(PROJECT_NAME) v$(VERSION) 构建配置"
	@echo "==============================================="
	@echo "项目名称: $(PROJECT_NAME)"
	@echo "版本: $(VERSION)"
	@echo "源码目录: $(SRC_DIR)"
	@echo "构建目录: $(BUILD_DIR)"
	@echo "目标文件: $(TARGET)"
	@echo ""
	@echo "编译器: $(CC)"
	@echo "编译选项: $(CFLAGS)"
	@echo "链接选项: $(LDFLAGS)"
	@echo ""
	@echo "功能配置:"
	@echo "  USE_SYSTEM_COMMANDS: $(USE_SYSTEM_COMMANDS)"
	@echo "  USE_SMARTCTL: $(USE_SMARTCTL)"
	@echo "  USE_NVME_CLI: $(USE_NVME_CLI)"
	@echo ""
	@echo "源文件数量: $(words $(ALL_SRCS))"
	@echo "==============================================="

# 帮助信息
help:
	@echo "$(PROJECT_NAME) v$(VERSION) 构建系统"
	@echo ""
	@echo "可用目标:"
	@echo "  all                - 构建程序 (默认)"
	@echo "  clean              - 清理构建文件"
	@echo "  install            - 安装到系统 (需要 sudo)"
	@echo "  uninstall          - 从系统卸载 (需要 sudo)"
	@echo ""
	@echo "构建变体:"
	@echo "  system-cmd-version - 构建系统命令版本 (推荐)"
	@echo "  static-version     - 构建静态 API 版本"
	@echo "  debug              - 构建调试版本"
	@echo "  release            - 构建发布版本"
	@echo ""
	@echo "配置选项:"
	@echo "  USE_SYSTEM_COMMANDS=1|0  - 使用系统命令或 C API"
	@echo "  USE_SMARTCTL=1|0         - 启用/禁用 smartctl"
	@echo "  USE_NVME_CLI=1|0         - 启用/禁用 nvme-cli"
	@echo ""
	@echo "示例:"
	@echo "  make                     - 使用默认配置构建"
	@echo "  make system-cmd-version  - 构建系统命令版本"
	@echo "  make debug               - 构建调试版本"
	@echo "  sudo make install        - 安装程序"
	@echo "  make config              - 显示当前配置"

# 确保目标不会与同名文件冲突
.PHONY: $(TARGET)

# 依赖关系检查
$(BUILD_DIR)/main.o: $(SRC_DIR)/main.c $(SRC_DIR)/device_info/device_info.h $(SRC_DIR)/scanner.h $(SRC_DIR)/scan_options.h $(SRC_DIR)/time_categories.h

$(BUILD_DIR)/scanner.o: $(SRC_DIR)/scanner.c $(SRC_DIR)/scanner.h $(SRC_DIR)/device_info/device_info.h $(SRC_DIR)/time_categories.h

$(BUILD_DIR)/scan_options.o: $(SRC_DIR)/scan_options.c $(SRC_DIR)/scan_options.h

$(BUILD_DIR)/time_categories.o: $(SRC_DIR)/time_categories.c $(SRC_DIR)/time_categories.h $(SRC_DIR)/device_info/device_info.h

$(BUILD_DIR)/device_info/device_info.o: $(DEVICE_INFO_DIR)/device_info.c $(DEVICE_INFO_DIR)/device_info.h

$(BUILD_DIR)/device_info/generic_info.o: $(DEVICE_INFO_DIR)/generic_info.c $(DEVICE_INFO_DIR)/generic_info.h $(DEVICE_INFO_DIR)/device_info.h

$(BUILD_DIR)/device_info/sata_info.o: $(DEVICE_INFO_DIR)/sata_info.c $(DEVICE_INFO_DIR)/sata_info.h $(DEVICE_INFO_DIR)/device_info.h

$(BUILD_DIR)/device_info/nvme_info.o: $(DEVICE_INFO_DIR)/nvme_info.c $(DEVICE_INFO_DIR)/nvme_info.h $(DEVICE_INFO_DIR)/device_info.h

$(BUILD_DIR)/device_info/usb_info.o: $(DEVICE_INFO_DIR)/usb_info.c $(DEVICE_INFO_DIR)/usb_info.h $(DEVICE_INFO_DIR)/device_info.h
