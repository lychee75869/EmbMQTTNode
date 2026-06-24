/*
 * mqtt_client.h / mqtt_client.c
 * MQTT 客户端封装，基于 libmosquitto
 * 阶段一增强：MQTT over TLS、设备身份、Last Will 遗嘱
 */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "common.h"

/* 初始化 MQTT 连接（含 TLS + 遗嘱支持） */
int mqtt_init(const char *host, int port,
              const char *client_id,
              const struct tls_config *tls,
              const char *will_topic,
              const char *will_payload);

/* 发布 JSON 格式传感器数据 */
int mqtt_publish(const struct node_config *cfg, const struct sensor_data *data);

/* 发布设备状态（online / offline / heartbeat） */
int mqtt_publish_status(const struct node_config *cfg,
                        const struct device_info *dev,
                        const char *status);

/* 订阅 OTA 升级指令主题 */
int mqtt_subscribe_ota(const char *client_id);

/* 检查当前是否连接 */
int mqtt_is_connected(void);

/* 循环处理网络事件（非阻塞，需定期调用） */
void mqtt_loop(int timeout_ms);

/* 关闭 MQTT 连接 */
void mqtt_close(void);

#endif /* MQTT_CLIENT_H */