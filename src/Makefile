# EmbMQTTNode Makefile
# 构建嵌入式 MQTT 边缘网关
# 阶段二：条件编译 Modbus 支持

CC       ?= gcc
CFLAGS   ?= -std=c11 -Wall -Wextra -O2 -g -pthread
LDFLAGS  ?= -pthread -lmosquitto -lsqlite3 -lm

# Modbus 可选模块（默认启用）
BUILD_WITH_MODBUS ?= 1

ifeq ($(BUILD_WITH_MODBUS),1)
    CFLAGS  += -DBUILD_WITH_MODBUS
    LDFLAGS += -lmodbus
    MODBUS_SRC = modbus_master.c
else
    MODBUS_SRC =
endif

TARGET   = embmqttnode
SRCS     = main.c config.c sensor.c storage.c mqtt_client.c daemon.c \
           $(MODBUS_SRC)
OBJS     = $(SRCS:.c=.o)

.PHONY: all clean strip install

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

strip: $(TARGET)
	strip --strip-unneeded $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)/usr/bin
	install -m 755 $(TARGET) $(DESTDIR)/usr/bin/
	install -d $(DESTDIR)/etc/embmqttnode
	install -m 644 ../config/node.conf $(DESTDIR)/etc/embmqttnode/

clean:
	rm -f $(OBJS) $(TARGET)
