# EmbMQTTNode 顶层 Makefile
# 委托 src/ 和 tests/ 子目录构建
# 阶段六：交叉编译 + CI + 安装

CROSS_COMPILE     ?=
BUILD_WITH_MODBUS ?= 1
DESTDIR           ?=

export CROSS_COMPILE
export BUILD_WITH_MODBUS
export DESTDIR

.PHONY: all clean test strip install dist distclean

all:
	$(MAKE) -C src

strip:
	$(MAKE) -C src strip

install:
	$(MAKE) -C src install

test:
	$(MAKE) -C tests
	@echo "=== Running all tests ==="
	@cd tests && \
		./test_sensor && \
		./test_storage && \
		./test_modbus_config && \
		./test_rule_engine && \
		./test_ota && \
		./test_anomaly_engine
	@echo "=== All 6 tests passed ==="

clean:
	$(MAKE) -C src clean
	$(MAKE) -C tests clean

distclean:
	$(MAKE) -C src distclean
	$(MAKE) -C tests distclean

dist: all strip
	@VER=$$(grep -oP 'EMBMQTTNODE_VERSION\s+"\K[^"]*' src/common.h); \
	ARCH=$$(uname -m); \
	mkdir -p dist; \
	tar czf dist/embmqttnode_$${VER}_$${ARCH}.tar.gz \
		src/embmqttnode config/node.conf config/embmqttnode.service; \
	echo "=== Release: dist/embmqttnode_$${VER}_$${ARCH}.tar.gz ==="
