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

TESTS   = test_sensor test_storage test_modbus_config test_rule_engine test_ota test_anomaly_engine test_mac_addr test_mqtt_client test_subdev_registry test_sensor_fields test_platform_local

.PHONY: all clean distclean

all: $(TESTS)

test_sensor: test_sensor.c ../src/sensor.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

test_storage: test_storage.c ../src/storage.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# v1.3.0 T01：追加链接 modbus_master.c，覆盖 mock 路径哨兵 + source_id 打标
# v1.3.0 T06：config.c / modbus_master.c 依赖字段描述表 → 链接 sensor_fields.c
test_modbus_config: test_modbus_config.c ../src/config.c ../src/modbus_master.c \
                    ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# v1.3.0 T01：子设备注册表解析/校验纯函数单测
test_subdev_registry: test_subdev_registry.c ../src/subdev_registry.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# v1.3.0 T06：rule_engine.c / config.c 依赖字段描述表 → 链接 sensor_fields.c
test_rule_engine: test_rule_engine.c ../src/rule_engine.c ../src/config.c \
                  ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# v1.3.0 T06：config.c 依赖字段描述表 → 链接 sensor_fields.c
test_ota: test_ota.c ../src/ota.c ../src/config.c ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lcrypto -lssl

# v1.3.0 T06：anomaly_engine.c / config.c 依赖字段描述表 → 链接 sensor_fields.c
test_anomaly_engine: test_anomaly_engine.c ../src/anomaly_engine.c \
                     ../src/config.c ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

test_mac_addr: test_mac_addr.c ../src/mac_addr.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# P1-13/P2-19: mqtt_client 回调调度 + 主题构造单测。
# v1.2.9 P1-9/P1-6: 追加 payload 构造器（mqtt_build_*_payload）与
# 常量时间 token 比较（http_consttime_token_equal，位于 http_server.c）
# 的用例——链接 http_server.c 及其依赖（storage/rule_engine/
# anomaly_engine/ota），不触碰真实网络；-lmosquitto 仅为解析
# mqtt_client.c 其余符号。
# v1.3.0 T02：mqtt_client.c 的 on_message/on_connect 改走平台分发器
# （platform_dispatch_message / platform_on_connected / platform_on_disconnected
# / platform_connect_params），故追加链接 platform.c + platform_local.c
# （分发器 + local 实现；local 实现依赖 ota.c/mqtt_client.c，均已在链上）。
test_mqtt_client: test_mqtt_client.c ../src/mqtt_client.c ../src/http_server.c \
                  ../src/storage.c ../src/rule_engine.c ../src/anomaly_engine.c \
                  ../src/ota.c ../src/sensor_fields.c ../src/platform.c \
                  ../src/platform_local.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lmosquitto -lcrypto -lssl

# v1.3.0 T06：字段描述表跨层重构单测（表序/存取/黄金 payload/config 校验）。
# 链接收敛为最小闭包：只用到 mqtt_build_data_payload（mqtt_client.c）与
# config_load（config.c）；mqtt_client.c 对 ota/storage/http/引擎零引用
# （OTA 走回调指针）。LOG_* 是 common.h 里的 #define 宏（header-only，
# 无独立 log.c TU），故无需额外链入。-lmosquitto 仅为解析 mqtt_client.c
# 其余符号；http_server.c 引 netinet/in.h，已从依赖中移除以保可移植性。
# v1.3.0 T02：mqtt_client.c 现依赖平台分发器（platform_connect_params/
# platform_on_connected/platform_dispatch_message/platform_on_disconnected），
# 故闭包追加 platform.c + platform_local.c；platform_local.c 又引用
# ota_handle_message → 追加 ota.c 及其 -lcrypto -lssl。
test_sensor_fields: test_sensor_fields.c ../src/sensor_fields.c \
                    ../src/mqtt_client.c ../src/config.c \
                    ../src/platform.c ../src/platform_local.c ../src/ota.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lmosquitto -lcrypto -lssl

# v1.3.0 T02：平台适配层单测（connect_params 纯函数 + select 装配 +
# 唯一路由/下行分发 seam）。链接 platform.c（分发器）+ platform_local.c
# （local 实现）+ 其依赖 mqtt_client.c（publish/subscribe 桩）/ ota.c
# （下行终点）/ config.c + sensor_fields.c（config/ota/mqtt 皆引用字段表）。
# 不触碰真实网络：mqtt_subscribe_ota/publish 在无 broker 时安全返回 E_INVAL。
test_platform_local: test_platform_local.c ../src/platform.c ../src/platform_local.c \
                     ../src/mqtt_client.c ../src/ota.c ../src/config.c \
                     ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lmosquitto -lcrypto -lssl

clean:
	rm -f $(TESTS) test_data.db test_modbus_tmp.conf test_legacy.db
	rm -rf test_mac_root*

distclean: clean
	rm -f $(TESTS:=.d)

-include $(TESTS:=.d)
