# SPDX-License-Identifier: GPL-2.0
# Makefile for phantom_lkm kernel module
# 教学研究用内核模块 - 链表动态摘除与sysfs节点管理
#
# 目标平台: OnePlus ACE5 (SM8650), 内核 6.1.141, Android 14 GKI

# 模块名称
MODULE_NAME := phantom_lkm

# 源文件
obj-m += $(MODULE_NAME).o

# 默认内核源码路径（可通过KDIR参数覆盖）
KDIR ?= /lib/modules/$(shell uname -r)/build

# 交叉编译支持（用于Android GKI内核）
# 使用示例: make CROSS_COMPILE=aarch64-linux-android- ARCH=arm64
CROSS_COMPILE ?=
ARCH ?= $(shell uname -m)

# 编译标志
ccflags-y += -Wall -Wextra -Wno-unused-parameter
ccflags-y += -O2
ccflags-y += -g  # 保留调试信息

# GKI合规性检查标志（可选）
# ccflags-y += -Werror=implicit-function-declaration

# 外部模块编译
all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

# 清理
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) -f *.o *.ko *.mod.c *.mod *.order *.symvers .*.cmd

# 深度清理（包括备份文件）
distclean: clean
	$(RM) -f *~ *.bak .*.swp .*.swo

# 安装模块（需要root权限）
install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

# 加载模块（需要root权限）
load:
	insmod $(MODULE_NAME).ko

# 卸载模块（需要root权限）
unload:
	rmmod $(MODULE_NAME)

# 查看trace输出
view-trace:
	@echo "=== Trace output ==="
	@cat /sys/kernel/tracing/trace | grep phantom_lkm || echo "No trace output found"

# 清空trace缓冲区
clear-trace:
	@echo > /sys/kernel/tracing/trace

# 帮助信息
help:
	@echo "Usage: make [target] [KDIR=/path/to/kernel]"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build the kernel module (default)"
	@echo "  clean        - Remove build artifacts"
	@echo "  distclean    - Remove all generated and backup files"
	@echo "  install      - Install module to system"
	@echo "  load         - Load the module (requires root)"
	@echo "  unload       - Unload the module (requires root)"
	@echo "  view-trace   - View trace_printk output"
	@echo "  clear-trace  - Clear trace buffer"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR         - Kernel source directory (default: /lib/modules/\$$(uname -r)/build)"
	@echo "  ARCH         - Target architecture (default: current arch)"
	@echo "  CROSS_COMPILE- Cross compiler prefix (default: empty)"
	@echo ""
	@echo "Examples:"
	@echo "  # Build for current kernel"
	@echo "  make"
	@echo ""
	@echo "  # Build for Android GKI kernel"
	@echo "  make KDIR=/path/to/android-kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-"
	@echo ""
	@echo "  # Build for OnePlus ACE5 (SM8650) kernel"
	@echo "  make KDIR=/path/to/sm8650-kernel ARCH=arm64 CROSS_COMPILE=aarch64-linux-android-"

.PHONY: all clean distclean install load unload view-trace clear-trace help
