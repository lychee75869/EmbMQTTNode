# EmbMQTTNode tests/Makefile
# 单元测试构建

CROSS_COMPILE ?=
CC      ?= $(CROSS_COMPILE)gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -g -pthread -I../src
LDFLAGS ?= -pthread -lsqlite3 -lm

# Modbus 可选模块
BUILD_WITH_MODBUS ?= 1
ifeq ($(BUILD_WITH_MODBUS),1)
    CFLAGS += -DBUILD_WITH_MODBUS
    LDFLAGS += -lmodbus
endif

TESTS   = test_sensor test_storage test_modbus_config test_rule_engine test_ota test_anomaly_engine test_mac_addr test_mqtt_client

.PHONY: all clean distclean

all: $(TESTS)

test_sensor: test_sensor.c ../src/sensor.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

test_storage: test_storage.c ../src/storage.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

test_modbus_config: test_modbus_config.c ../src/config.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

test_rule_engine: test_rule_engine.c ../src/rule_engine.c ../src/config.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

test_ota: test_ota.c ../src/ota.c ../src/config.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lcrypto

test_anomaly_engine: test_anomaly_engine.c ../src/anomaly_engine.c ../src/config.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

test_mac_addr: test_mac_addr.c ../src/mac_addr.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# P1-13/P2-19: mqtt_client 回调调度 + 主题构造单测。
# 只测纯函数化路径（mqtt_handle_connack / mqtt_build_*_topic），
# 不触碰真实网络；链接 -lmosquitto 仅为解析 mqtt_client.c 其余符号。
test_mqtt_client: test_mqtt_client.c ../src/mqtt_client.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lmosquitto

clean:
	rm -f $(TESTS) test_data.db test_modbus_tmp.conf
	rm -rf test_mac_root*

distclean: clean
	rm -f $(TESTS:=.d)

-include $(TESTS:=.d)
