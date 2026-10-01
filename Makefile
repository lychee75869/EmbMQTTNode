# 物种感知智能灌溉系统 —— 仓库根 Makefile
#
# 本仓库为四模块结构（子设备 / 网关 / 云端 / 小程序），其中只有网关是
# make 工程。根 Makefile 只做一件事：把构建、测试、打包转发给 gateway/，
# 让仓库顶层保留「一条命令即可构建并验证网关」的入口。
#
# 子设备固件在 subdevice/，是 Keil MDK 工程，不走 make：
#   Keil uVision5 打开 subdevice/Projects/MDK-ARM/auto_dwater(2).uvprojx

GATEWAY_DIR ?= gateway

CROSS_COMPILE     ?=
BUILD_WITH_MODBUS ?= 1
DESTDIR           ?=

export CROSS_COMPILE
export BUILD_WITH_MODBUS
export DESTDIR

.PHONY: all test strip install dist clean distclean help

all:
	$(MAKE) -C $(GATEWAY_DIR)

test:
	$(MAKE) -C $(GATEWAY_DIR) test

strip:
	$(MAKE) -C $(GATEWAY_DIR) strip

install:
	$(MAKE) -C $(GATEWAY_DIR) install

dist:
	$(MAKE) -C $(GATEWAY_DIR) dist

clean:
	$(MAKE) -C $(GATEWAY_DIR) clean

distclean:
	$(MAKE) -C $(GATEWAY_DIR) distclean

help:
	@echo "物种感知智能灌溉系统 —— 根 Makefile（转发到 $(GATEWAY_DIR)/）"
	@echo ""
	@echo "  make                               构建网关（含 Modbus，默认 BUILD_WITH_MODBUS=1）"
	@echo "  make BUILD_WITH_MODBUS=0           构建网关（不含 Modbus）"
	@echo "  make CROSS_COMPILE=aarch64-linux-gnu-   交叉编译 ARM64"
	@echo "  make test                          编译并运行全部单元测试"
	@echo "  make strip                         剥离调试符号"
	@echo "  make install DESTDIR=/path/rootfs  安装到目标根文件系统"
	@echo "  make dist                          打包 dist/embmqttnode_<ver>_<arch>.tar.gz"
	@echo "  make clean                         清理编译产物"
	@echo "  make distclean                     连 .d 依赖文件一起清理"
	@echo ""
	@echo "子设备固件（subdevice/）为 Keil MDK 工程，请在 Keil uVision 中构建。"
