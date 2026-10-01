/*
 * mqtt_client.h / mqtt_client.c —— MQTT 客户端封装（libmosquitto）
 * 支持 MQTT over TLS、设备身份、Last Will 遗嘱。
 *
 * 传输层与平台解耦（勿在传输层重新引入平台分支）：
 *   - mqtt_init 经 platform_connect_params 取连接参数（client_id/username/
 *     password/keepalive/use_will/TLS 锚点/CA），host/port 取自 cfg
 *   - TLS 分支由 connect_params.force_tls || cfg->tls.enabled 驱动
 *   - on_message 全量转发 platform_dispatch_message（不判 topic）
 *   - on_connect 触发 platform_on_connected（重订阅/重发状态由平台实现）
 */
#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include "common.h"

/* 初始化 MQTT 连接（含 TLS + 遗嘱支持） */
int mqtt_init(const struct node_config *cfg);

/* 发布 JSON 格式传感器数据 */
int mqtt_publish(const struct node_config *cfg, const struct sensor_data *data);

/* 发布自定义 payload 到指定 topic（用于 Modbus 等扩展模块） */
int mqtt_publish_raw(const char *topic, const char *payload, int qos);

/* 发布设备状态（online / offline / heartbeat） */
int mqtt_publish_status(const struct node_config *cfg,
                        const struct device_info *dev,
                        const char *status);

/* 订阅 OTA 升级指令主题（local 重订用："embmqttnode/<client_id>/ota/cmd"） */
int mqtt_subscribe_ota(const char *client_id);

/* 订阅任意主题（薄封装 mosquitto_subscribe + 日志） */
int mqtt_subscribe_topic(const char *topic, int qos);

/*
 * "连接成功"回调类型。每次收到成功 CONNACK（含首次连接与断网自动重连）时在
 * libmosquitto 网络线程中被调用；回调内可安全调用 mqtt_publish_* /
 * mqtt_subscribe_ota（libmosquitto 官方允许在回调线程使用这些线程安全接口）。
 */
typedef void (*mqtt_connected_callback)(void);

/*
 * 注册"连接成功"回调，传 NULL 清除。必须在 mqtt_init 之前注册——连接是异步
 * 建立的（mosquitto_connect + loop_start），否则存在"首次 CONNACK 先于注册
 * 到达"的窗口。
 */
void mqtt_set_connected_callback(mqtt_connected_callback cb);

/*
 * CONNACK 处理逻辑（on_connect 回调体）。
 * rc==0：置连接标志 → platform_on_connected(cfg) → 触发已注册的成功回调；
 * rc!=0：清连接标志 → platform_on_disconnected() → 打日志并返回 E_NET。
 * 单独导出供单测覆盖注册/判空/重连重复触发（无需真实 broker）。
 */
int mqtt_handle_connack(int rc);

/*
 * OTA 订阅主题构造（纯函数）："embmqttnode/<client_id>/ota/cmd"。
 * 参数非法返回 E_INVAL；按 snprintf 语义安全截断（不越界，保证 null 结尾）。
 */
int mqtt_build_ota_topic(const char *client_id, char *buf, int buf_len);

/* 状态主题构造（纯函数）："<base_topic>/status"，语义同上 */
int mqtt_build_status_topic(const char *base_topic, char *buf, int buf_len);

/*
 * 传感器数据 payload 构造（纯函数）：
 * {"client_id","timestamp","temperature","humidity","pressure"}。
 * 参数非法返回 E_INVAL；snprintf 截断（n < 0 || n >= buf_len）返回 E_IO，
 * 上层据此拒绝发布半截 JSON。
 */
int mqtt_build_data_payload(const struct node_config *cfg,
                            const struct sensor_data *data,
                            char *buf, int buf_len);

/*
 * 设备状态 payload 构造（纯函数）：
 * {"client_id","status","version","hostname","mac","cpu","kernel","mem_kb",
 *  "timestamp"}。返回值语义同上。
 */
int mqtt_build_status_payload(const struct node_config *cfg,
                              const struct device_info *dev,
                              const char *status,
                              char *buf, int buf_len);

/* 检查当前是否连接 */
int mqtt_is_connected(void);

/* 循环处理网络事件（非阻塞，需定期调用） */
void mqtt_loop(int timeout_ms);

/*
 * 会话丢失去重分发（CONNACK 失败与 on_disconnect 共用）：仅当本次会话曾成功
 * 建立时才通知 platform_on_disconnected() 一次，否则 no-op。
 * 返回 1=真实触发；0=去重/从未连上。导出供单测覆盖去重语义。
 */
int mqtt_on_link_lost(void);

/*
 * auth_type=1 的超窗重连监督线程：
 *   mqtt_start_supervisor   仅 rebuild_on_hour==1 才创建线程，否则 no-op（E_OK）
 *   mqtt_stop_supervisor    置 0 并 join（未启动时为 no-op）
 *   mqtt_rebuild            销毁→按新时间戳重建实例（与 mqtt_close 共享锁互斥）
 *   mqtt_rebuild_needed     纯函数窗口判定：built_ts>0 且 now-built_ts>=1800
 */
int  mqtt_start_supervisor(void);
void mqtt_stop_supervisor(void);
int  mqtt_rebuild(void);
int  mqtt_rebuild_needed(int64_t now_epoch, int64_t built_ts);

/* 关闭 MQTT 连接 */
void mqtt_close(void);

#endif /* MQTT_CLIENT_H */
