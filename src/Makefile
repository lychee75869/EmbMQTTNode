# EmbMQTTNode Makefile
# 构建嵌入式 MQTT 环境监测节点

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -g -pthread
LDFLAGS ?= -pthread -lmosquitto -lsqlite3 -lm

TARGET  = embmqttnode
SRCS    = $(wildcard *.c)
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
