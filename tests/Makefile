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

TESTS   = test_sensor test_storage test_modbus_config test_rule_engine test_ota test_anomaly_engine

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

clean:
	rm -f $(TESTS) test_data.db test_modbus_tmp.conf

distclean: clean
	rm -f $(TESTS:=.d)

-include $(TESTS:=.d)
