/*
 * mqtt_client.h / mqtt_client.c
 * MQTT 客户端封装，基于 libmosquitto
 */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "common.h"

/* 初始化 MQTT 连接 */
int mqtt_init(const char *host, int port, const char *client_id);

/* 发布 JSON 格式数据 */
int mqtt_publish(const struct node_config *cfg, const struct sensor_data *data);

/* 检查当前是否连接 */
int mqtt_is_connected(void);

/* 循环处理网络事件（非阻塞，需定期调用） */
void mqtt_loop(int timeout_ms);

/* 关闭 MQTT 连接 */
void mqtt_close(void);

#endif /* MQTT_CLIENT_H */
