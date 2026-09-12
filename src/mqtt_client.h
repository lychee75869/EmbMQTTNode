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

/* 发布自定义 payload 到指定 topic（用于 Modbus 等扩展模块） */
int mqtt_publish_raw(const char *topic, const char *payload, int qos);

/* 发布设备状态（online / offline / heartbeat） */
int mqtt_publish_status(const struct node_config *cfg,
                        const struct device_info *dev,
                        const char *status);

/* 订阅 OTA 升级指令主题 */
int mqtt_subscribe_ota(const char *client_id);

/*
 * OTA 消息回调类型
 * payload:     消息体
 * payload_len: 消息长度
 */
typedef void (*mqtt_ota_callback)(const char *payload, int payload_len);

/*
 * 注册 OTA 消息回调（收到 ota/cmd 消息时调用）
 */
void mqtt_set_ota_callback(mqtt_ota_callback cb);

/*
 * P1-13/P2-19: "连接成功"回调类型（无参数，无返回值）。
 * 每次收到成功 CONNACK（含首次连接与断网自动重连）时，
 * 在 libmosquitto 网络线程中被调用。回调内可安全调用
 * mqtt_publish_* / mqtt_subscribe_ota（libmosquitto 官方
 * 允许在回调线程使用这些线程安全接口）。
 */
typedef void (*mqtt_connected_callback)(void);

/*
 * P1-13/P2-19: 注册"连接成功"回调。传 NULL 清除。
 * 必须在 mqtt_init 之前注册，否则存在"首次 CONNACK 先于
 * 注册到达"的窗口（连接是异步建立的，见 mqtt_init 内
 * mosquitto_connect + loop_start）。
 */
void mqtt_set_connected_callback(mqtt_connected_callback cb);

/*
 * P1-13/P2-19: CONNACK 处理逻辑（on_connect 回调的函数体）。
 * rc==0: 置连接标志并调用已注册的连接成功回调（重订阅/重发状态）；
 * rc!=0: 清连接标志并按返回码打日志，返回 E_NET。
 * 从 on_connect（网络线程）调用；单独导出供单元测试覆盖
 * 回调注册/判空/重连重复触发逻辑（无需真实 broker）。
 */
int mqtt_handle_connack(int rc);

/*
 * P1-13/P2-19: OTA 订阅主题构造（纯函数，供单元测试）：
 * "embmqttnode/<client_id>/ota/cmd"。参数非法返回 E_INVAL；
 * 缓冲区不足时按 snprintf 语义安全截断（不越界，保证 null 结尾）。
 */
int mqtt_build_ota_topic(const char *client_id, char *buf, int buf_len);

/*
 * P1-13/P2-19: 状态主题构造（纯函数，供单元测试）：
 * "<base_topic>/status"。参数与截断语义同上。
 */
int mqtt_build_status_topic(const char *base_topic, char *buf, int buf_len);

/*
 * P1-9: 传感器数据 payload 构造（纯函数，供单元测试）：
 * {"client_id","timestamp","temperature","humidity","pressure"}。
 * 参数非法返回 E_INVAL；snprintf 截断（n < 0 || n >= buf_len）
 * 返回 E_IO——上层据此拒绝发布半截 JSON。
 */
int mqtt_build_data_payload(const struct node_config *cfg,
                            const struct sensor_data *data,
                            char *buf, int buf_len);

/*
 * P1-9: 设备状态 payload 构造（纯函数，供单元测试）：
 * {"client_id","status","version","hostname","mac","cpu",
 *  "kernel","mem_kb","timestamp"}。返回值语义同上。
 */
int mqtt_build_status_payload(const struct node_config *cfg,
                              const struct device_info *dev,
                              const char *status,
                              char *buf, int buf_len);

/* 检查当前是否连接 */
int mqtt_is_connected(void);

/* 循环处理网络事件（非阻塞，需定期调用） */
void mqtt_loop(int timeout_ms);

/* 关闭 MQTT 连接 */
void mqtt_close(void);

#endif /* MQTT_CLIENT_H */