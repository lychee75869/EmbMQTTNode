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

TESTS   = test_storage test_modbus_config test_ota test_mac_addr \
         test_mqtt_client test_subdev_registry test_sensor_fields \
         test_platform_local test_platform_huawei test_platform_huawei_subdev \
         test_platform_huawei_cmd

.PHONY: all clean distclean

all: $(TESTS)

test_storage: test_storage.c ../src/storage.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# 追加链接 modbus_master.c，覆盖 mock 路径哨兵 + source_id 打标
# config.c / modbus_master.c 依赖字段描述表 → 链接 sensor_fields.c
test_modbus_config: test_modbus_config.c ../src/config.c ../src/modbus_master.c \
                    ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# 子设备注册表解析/校验纯函数单测
test_subdev_registry: test_subdev_registry.c ../src/subdev_registry.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# config.c 依赖字段描述表 → 链接 sensor_fields.c
test_ota: test_ota.c ../src/ota.c ../src/config.c ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lcrypto -lssl

test_mac_addr: test_mac_addr.c ../src/mac_addr.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS)

# 追加 payload 构造器（mqtt_build_*_payload）与
# 常量时间 token 比较（http_consttime_token_equal，位于 http_server.c）
# 的用例——链接 http_server.c 及其依赖（storage/ota），不触碰真实网络；
# -lmosquitto 仅为解析 mqtt_client.c 其余符号。
# mqtt_client.c 的 on_message/on_connect 改走平台分发器
# （platform_dispatch_message / platform_on_connected / platform_on_disconnected
# / platform_connect_params），故追加链接 platform.c + platform_local.c
# （分发器 + local 实现；local 实现依赖 ota.c/mqtt_client.c，均已在链上）。
# platform.c 的 huawei 分支现引用 hw_connect_params/
# hw_subdev_init/platform_huawei_ops → 闭包追加 platform_huawei.c +
# platform_huawei_subdev.c（ops 表 + 其依赖 subdev_registry.c 的
# subdev_load/at/find）。三者符号均已满足：mqtt_client.c（publish 桩）、
# sensor_fields.c（builders 字段表）、-lcrypto（HMAC）已在链上。
# platform_huawei_subdev.c 现引用 hw_cmd_handle → 追加
# platform_huawei_cmd.c（其依赖 ota.c/platform_huawei.c 均已在链上）。
test_mqtt_client: test_mqtt_client.c ../src/mqtt_client.c ../src/http_server.c ../src/storage.c  \
                  ../src/ota.c ../src/sensor_fields.c ../src/platform.c \
                  ../src/platform_local.c ../src/platform_huawei.c \
                  ../src/platform_huawei_subdev.c ../src/platform_huawei_cmd.c \
                  ../src/subdev_registry.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lmosquitto -lcrypto -lssl

# 字段描述表跨层重构单测（表序/存取/黄金 payload/config 校验）。
# 链接收敛为最小闭包：只用到 mqtt_build_data_payload（mqtt_client.c）与
# config_load（config.c）；mqtt_client.c 对 ota/storage/http/引擎零引用
# （OTA 走回调指针）。LOG_* 是 common.h 里的 #define 宏（header-only，
# 无独立 log.c TU），故无需额外链入。-lmosquitto 仅为解析 mqtt_client.c
# 其余符号；http_server.c 引 netinet/in.h，已从依赖中移除以保可移植性。
# mqtt_client.c 现依赖平台分发器（platform_connect_params/
# platform_on_connected/platform_dispatch_message/platform_on_disconnected），
# 故闭包追加 platform.c + platform_local.c；platform_local.c 又引用
# ota_handle_message → 追加 ota.c 及其 -lcrypto -lssl。
# 同理补 huawei 闭包（platform_huawei.c + _subdev.c + subdev_registry.c）。
# platform_huawei_subdev.c 引 hw_cmd_handle → 再补 platform_huawei_cmd.c。
test_sensor_fields: test_sensor_fields.c ../src/sensor_fields.c \
                    ../src/mqtt_client.c ../src/config.c \
                    ../src/platform.c ../src/platform_local.c ../src/ota.c \
                    ../src/platform_huawei.c ../src/platform_huawei_subdev.c \
                    ../src/platform_huawei_cmd.c ../src/subdev_registry.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lmosquitto -lcrypto -lssl

# 平台适配层单测（connect_params 纯函数 + select 装配 +
# 唯一路由/下行分发 seam）。链接 platform.c（分发器）+ platform_local.c
# （local 实现）+ 其依赖 mqtt_client.c（publish/subscribe 桩）/ ota.c
# （下行终点）/ config.c + sensor_fields.c（config/ota/mqtt 皆引用字段表）。
# 不触碰真实网络：mqtt_subscribe_ota/publish 在无 broker 时安全返回 E_INVAL。
test_platform_local: test_platform_local.c ../src/platform.c ../src/platform_local.c \
                     ../src/mqtt_client.c ../src/ota.c ../src/config.c \
                     ../src/sensor_fields.c ../src/platform_huawei.c \
                     ../src/platform_huawei_subdev.c ../src/platform_huawei_cmd.c \
                     ../src/subdev_registry.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lmosquitto -lcrypto -lssl

# huawei 纯函数单测（client_id/password(HMAC)/keepalive/时间/
# 主题分类/payload builders/connect_params）。纯函数无外部依赖：仅需
# platform_huawei.c + sensor_fields.c（字段表 for_each_field）与 libcrypto
# （HMAC-SHA256）；不链接 mqtt/ota/sqlite。
test_platform_huawei: test_platform_huawei.c ../src/platform_huawei.c \
                      ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lcrypto

# huawei 子设备管理单测（count/at、init 对齐、publish_data
# 返回契约、status/alert、tick 退避、select 装配、注册响应）。链接平台
# 装配层 platform.c（分发器/select）+ 两个 ops 实现 platform_local.c 与
# platform_huawei_subdev.c（platform.c 同时引用两符号，闭包必须完整）+
# subdev_registry.c（count/at/find）+ 其依赖 mqtt_client.c（publish/
# subscribe 桩，测试用 hw_subdev_set_publisher 注入免 broker 发布器）/
# ota.c（platform_local.c 下行终点）/ config.c + sensor_fields.c（字段表）。
# 不触碰真实网络：无 broker 时 mqtt_subscribe_topic 安全返回 E_INVAL。
# platform_huawei_subdev.c 现引 hw_cmd_handle → 追加 platform_huawei_cmd.c。
test_platform_huawei_subdev: test_platform_huawei_subdev.c \
                     ../src/platform.c ../src/platform_local.c \
                     ../src/platform_huawei.c ../src/platform_huawei_subdev.c \
                     ../src/platform_huawei_cmd.c ../src/subdev_registry.c \
                     ../src/mqtt_client.c ../src/ota.c ../src/config.c \
                     ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lmosquitto -lcrypto -lssl

# 命令闭环单测（hw_cmd_parse 完整性矩阵 / 回执码与主题拼接 /
# hw_build_ota_status_event / hw_ota_status_shim 映射 / reboot 先回执后停机）。
# 链接：platform.c（node_config/reboot_request/ota_status_publish）+ 两 ops 实现
# （platform_local.c / platform_huawei_subdev.c 由 platform.c 引用）+ 
# platform_huawei_cmd.c（被测）+ huawei 纯函数 + subdev_registry.c + mqtt_client.c
# （发布桩，测试用 hw_cmd_set_publisher 注入免 broker 发布器）+ ota.c（桥接终点，
# 受理判定用 ota_state_string）+ config.c + sensor_fields.c。
test_platform_huawei_cmd: test_platform_huawei_cmd.c \
                     ../src/platform.c ../src/platform_local.c \
                     ../src/platform_huawei.c ../src/platform_huawei_subdev.c \
                     ../src/platform_huawei_cmd.c ../src/subdev_registry.c \
                     ../src/mqtt_client.c ../src/ota.c ../src/config.c \
                     ../src/sensor_fields.c
	$(CC) $(CFLAGS) -MMD -MP $^ -o $@ $(LDFLAGS) -lmosquitto -lcrypto -lssl \
	    -Wl,--wrap=ota_handle_message

clean:
	rm -f $(TESTS) test_data.db test_modbus_tmp.conf test_legacy.db
	rm -rf test_mac_root*

distclean: clean
	rm -f $(TESTS:=.d)

-include $(TESTS:=.d)
