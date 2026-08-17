# EmbMQTTNode Makefile
# 构建嵌入式 MQTT 边缘网关
# 阶段六：交叉编译 + 自动依赖 + CI

#变量定义
CROSS_COMPILE ?=
CC       ?= $(CROSS_COMPILE)gcc
STRIP    ?= $(CROSS_COMPILE)strip
CFLAGS   ?= -std=c11 -Wall -Wextra -O0 -g -pthread -D_DEFAULT_SOURCE
LDFLAGS  ?= -pthread -lmosquitto -lsqlite3 -lm -lcrypto

# Modbus 可选模块（条件编译，默认启用）
BUILD_WITH_MODBUS ?= 1

ifeq ($(BUILD_WITH_MODBUS),1)
    CFLAGS  += -DBUILD_WITH_MODBUS
    LDFLAGS += -lmodbus
endif

#源文件列表 + 自动推导
TARGET   = embmqttnode
SRCS     = main.c config.c sensor.c storage.c mqtt_client.c daemon.c \
            modbus_master.c rule_engine.c gpio_hal.c ota.c http_server.c \
            anomaly_engine.c
OBJS     = $(SRCS:.c=.o)
DEPS     = $(OBJS:.o=.d) #字符串替换语法：$(变量:旧后缀=新后缀)

.PHONY: all obj clean strip install distclean

all: $(TARGET)

obj: $(OBJS)

#链接规则
$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

#   - $(OBJS) — 所有.o 文件全部作为输入
#   - $@   $@ 是自动变量，展开为目标名 embmqttnode
#   - $(LDFLAGS) — 外部库放在最后（链接器的要求：库放在引用它们的 .o 之后）

#编译
%.o: %.c   #% 是通配符，意思是"任何 .o 文件都依赖同名的 .c 文件"
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
# 	$< — 自动变量，展开为第一个依赖（即 .c 文件）
#   $@ — 自动变量，展开为目标（即 .o 文件）
#   -c — "只编译不链接"，生成 .o 就停
#   -MMD -MP — 自动生成依赖文件 .d
#   -MMD：编译的同时生成 .d 文件，记录 #include 关系（不包含系统头文件如 <stdio.h>）
#   -MP：为每个头文件生成一个空的假目标，防止头文件被删除后 Make 报错

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
