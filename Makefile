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

# ── 测试用例清单（runner 单一事实源）──
# 与 tests/Makefile 的 TESTS 保持一致。用例数由 $(words $(TESTS)) 动态得出，
# 杜绝「编译了 N 个、runner 只跑 N-1 个、结尾计数还写死」的假绿复发。
TESTS = test_sensor test_storage test_modbus_config test_rule_engine test_ota \
        test_anomaly_engine test_mac_addr test_mqtt_client test_subdev_registry \
        test_sensor_fields test_platform_local test_platform_huawei \
        test_platform_huawei_subdev

test:
	$(MAKE) -C tests
	@echo "=== Running all tests ==="
	@cd tests && for t in $(TESTS); do \
		echo "--- $$t"; \
		./$$t || exit 1; \
	done
	@echo "=== All $(words $(TESTS)) tests passed ==="

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
		src/embmqttnode config/node.conf config/embmqttnode.service \
		config/embmqttnode-launcher config/subdevices.conf; \
	echo "=== Release: dist/embmqttnode_$${VER}_$${ARCH}.tar.gz ==="
