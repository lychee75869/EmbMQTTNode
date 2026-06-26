# EmbMQTTNode Makefile
# 构建嵌入式 MQTT 边缘网关
# 阶段六：交叉编译 + 自动依赖 + CI

CROSS_COMPILE ?=
CC       ?= $(CROSS_COMPILE)gcc
STRIP    ?= $(CROSS_COMPILE)strip
CFLAGS   ?= -std=c11 -Wall -Wextra -O2 -g -pthread -D_DEFAULT_SOURCE
LDFLAGS  ?= -pthread -lmosquitto -lsqlite3 -lm -lcrypto

# Modbus 可选模块（默认启用）
BUILD_WITH_MODBUS ?= 1

ifeq ($(BUILD_WITH_MODBUS),1)
    CFLAGS  += -DBUILD_WITH_MODBUS
    LDFLAGS += -lmodbus
endif

TARGET   = embmqttnode
SRCS     = main.c config.c sensor.c storage.c mqtt_client.c daemon.c \
           modbus_master.c rule_engine.c gpio_hal.c ota.c http_server.c
OBJS     = $(SRCS:.c=.o)
DEPS     = $(OBJS:.o=.d)

.PHONY: all obj clean strip install distclean

all: $(TARGET)

obj: $(OBJS)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

strip: $(TARGET)
	$(STRIP) --strip-unneeded $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/bin/
	install -d $(DESTDIR)/etc/embmqttnode
	install -m 644 ../config/node.conf $(DESTDIR)/etc/embmqttnode/

clean:
	rm -f $(OBJS) $(TARGET)

distclean: clean
	rm -f $(DEPS)

-include $(DEPS)
