/*
 * platform.h / platform.c
 * 平台适配层：平台无关分发器（docs/12 §1.2 / §3.1）。
 *
 * 目标：把"平台差异（连接参数组装 / 下行消息路由 / 上报路径）"收敛到
 * platform_ops 函数指针表之后；main / mqtt_client 等调用点零分支，
 * 新增平台只加一个 .c 并注册一张 ops 表，不动传输层。
 *
 * 装配与生命周期（§8 锁纪律）：
 *   - platform_select(cfg) 在所有线程创建之前由 main 调用一次，写定 g_active；
 *   - 此后 g_active 只读、无需原子/锁；各 API 均为对 g_active 的薄转发。
 *
 * local 路径（platform_local）行为与 v1.2.11 逐项等价；
 * huawei 实现（platform_huawei*）在 T03+ 落地。
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include "common.h"

/*
 * 每次建立连接前由 mqtt_client 调用，取"这次连接要用的参数"。
 * local 为静态取值；huawei 内含"现算"语义（时间戳 + HMAC，§4.1）。
 */
struct platform_connect_params {
    char client_id[320];  /* local: cfg->client_id；huawei: {id}_0_{auth}_{ts} */
    char username[288];   /* local: tls.username(可空)；huawei: device_id */
    char password[96];    /* local: tls.password；huawei: HMAC-SHA256 64hex+NUL */
    int  keepalive;       /* local: 60；huawei: 钳制后 30..1200 */
    int  force_tls;       /* 1=强制 TLS（huawei 恒 1）；mqtt_init 依此走 TLS 分支 */
    int  use_will;        /* local: 1；huawei: 0（平台不支持 will） */
    char will_topic[256];
    char will_payload[256];
    char ca_file[256];    /* TLS 锚点；huawei: 华为预置 CA */
    int  rebuild_on_hour; /* 1=auth_type 1：断开且超 ±30min 窗口需重建实例 */
    int  built_ts;        /* 生成 client_id 时的 UTC epoch（监督线程窗口判定用） */
};

/*
 * 平台操作表。所有成员均可为空实现（分发器判空转发）。
 * 线程归属见 docs/12 §8：on_connected/on_disconnected/on_message 运行在
 * libmosquitto 网络线程；publish_data/publish_alert 运行在采集线程；
 * tick 运行在 upload_thread。
 */
struct platform_ops {
    const char *name;
    int  (*connect_params)(const struct node_config *cfg,
                           struct platform_connect_params *out);
    int  (*on_connected)(const struct node_config *cfg);   /* CONNACK 成功 */
    void (*on_disconnected)(void);                          /* 意外断连：churn 检测 */
    void (*on_message)(const char *topic, const char *payload, int len);
    int  (*publish_status)(const struct node_config *cfg,
                           const struct device_info *dev, const char *status);
    int  (*publish_data)(const struct node_config *cfg,
                         const struct sensor_data *data);
    int  (*publish_alert)(const struct node_config *cfg,
                          const struct alert_event *evt);
    void (*tick)(const struct node_config *cfg);            /* upload_thread 周期维护 */
    void (*shutdown)(void);
};

/* 各平台 ops 表（由 platform_select 装配） */
extern const struct platform_ops platform_local_ops;
/* TODO(T04): extern const struct platform_ops platform_huawei_ops; */

/* ─── 分发器 API（均薄转发 g_active，启动后只读无锁）─────────── */

/* 选择平台并写定 g_active；须在任何线程创建之前调用一次。 */
int  platform_select(struct node_config *cfg);

int  platform_connect_params(const struct node_config *cfg,
                             struct platform_connect_params *out);
int  platform_publish_data(const struct node_config *cfg,
                           const struct sensor_data *data);
int  platform_publish_status(const struct node_config *cfg,
                             const struct device_info *dev, const char *status);
int  platform_publish_alert(const struct node_config *cfg,
                            const struct alert_event *evt);
void platform_dispatch_message(const char *topic, const char *payload, int len);
void platform_on_connected(const struct node_config *cfg);
void platform_on_disconnected(void);
void platform_tick(const struct node_config *cfg);

/*
 * 注册"重启请求"回调（main 注入 g_running=0）。
 * local 本轮无命令入口（huawei reboot 命令用，T05）。
 */
void platform_set_reboot_request(void (*cb)(void));
void (*platform_reboot_request(void))(void);

/*
 * 注入只读上下文（main 初始化期调用，线程创建前）：
 *   - device_info：local on_connected 发 online 状态所需（原为 main 的 g_dev）
 *   - node_config：由分发层保存，供无 cfg 入参的on_message/等取用
 *     （ops.on_message 签名冻结为 (topic,payload,len)，故需旁路取得 cfg）
 */
void platform_set_device_info(const struct device_info *dev);
const struct device_info *platform_device_info(void);
const struct node_config   *platform_node_config(void);

#endif /* PLATFORM_H */
