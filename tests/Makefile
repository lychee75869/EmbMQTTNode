# EmbMQTTNode tests/Makefile
# 单元测试构建

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -g -pthread -I../src
LDFLAGS ?= -pthread -lsqlite3 -lm

TESTS   = test_sensor test_storage test_modbus_config

.PHONY: all clean

all: $(TESTS)

test_sensor: test_sensor.c ../src/sensor.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_storage: test_storage.c ../src/storage.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

test_modbus_config: test_modbus_config.c ../src/config.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

clean:
	rm -f $(TESTS) test_data.db test_modbus_tmp.conf
