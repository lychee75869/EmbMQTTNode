/*
 * platform_local.c
 * local 平台实现（docs/12 §3.1 / §4.2 / §4.4）：v1.2.11 行为原样。
 *
 * 关键约束：本文件是 local 模式下 **唯一持有 ota/cmd 下行路由**的地方
 * （§3.1 唯一路由归属）：mqtt_client.on_message 不再判 topic，全量转发
 * platform_dispatch_message → 本文件 on_message 直连 ota_handle_message。
 * 因此任一模式有且只有一条下行路由，杜绝"双处理"。
 *
 * 行为等价基线：主题/payload/QoS/遗嘱/alert 串/断网续传 均与 v1.2.11 相同；
 * local 不做哨兵 mask（§3.5-4，显式边界）。
 */
#include "platform.h"
#include "mqtt_client.h"
#include "ota.h"

#include <string.h>
#include <time.h>

/* ─── 1. 连接参数组装（等价 v1.2.11 main.c 第 5 步 + mqtt_init 入参）──── */

static int local_connect_params(const struct node_config *cfg,
                                struct platform_connect_params *out)
{
    if (!cfg || !out)
        return E_INVAL;

    memset(out, 0, sizeof(*out));

    snprintf(out->client_id, sizeof(out->client_id), "%s", cfg->client_id);
    snprintf(out->username, sizeof(out->username), "%s", cfg->tls.username);
    snprintf(out->password, sizeof(out->password), "%s", cfg->tls.password);
    snprintf(out->ca_file, sizeof(out->ca_file), "%s", cfg->tls.ca_file);

    out->keepalive = 60;   /* v1.2.11 硬编码 60（P2-17 记载不可配），保持等价 */
    out->force_tls = 0;    /* local 的 TLS 完全由 cfg->tls.enabled 驱动 */
    out->use_will = 1;     /* v1.2.11 恒定设置遗嘱消息 */
    out->rebuild_on_hour = 0;  /* local 无需跨小时重建（§9-7） */
    out->built_ts = (int)time(NULL);

    /* 遗嘱 topic/payload：照搬 v1.2.11 main.c 原逻辑 */
    snprintf(out->will_topic, sizeof(out->will_topic),
             "%s/status", cfg->topic);
    snprintf(out->will_payload, sizeof(out->will_payload),
             "{\"client_id\":\"%s\",\"status\":\"offline\",\"timestamp\":%lld}",
             cfg->client_id, (long long)time(NULL) * 1000LL);

    return E_OK;
}

/* ─── 2. 连接成功（等价 v1.2.11 main.c on_mqtt_connected）────────────── */

static int local_on_connected(const struct node_config *cfg)
{
    if (!cfg)
        return E_INVAL;

    /* dev 由 main 经 platform_set_device_info 注入（原为 main 的 g_dev） */
    const struct device_info *dev = platform_device_info();
    if (dev) {
        if (mqtt_publish_status(cfg, dev, "online") != E_OK)
            LOG_WARN("publish online status failed on connect");
    } else {
        LOG_WARN("local on_connected: device_info not injected, "
                 "skip online status");
    }

    /* P1-13 核心：clean session 下 broker 已清订阅，必须重订 OTA 主题 */
    if (mqtt_subscribe_ota(cfg->client_id) != E_OK)
        LOG_WARN("subscribe ota topic failed on connect");

    return E_OK;
}

/* ─── 3. 断连（local 无额外副作用）─────────────────────────────────── */

static void local_on_disconnected(void)
{
    /* local 重连由 libmosquitto loop_start 自理（§9-7）；断连日志已由
     * mqtt_client.c on_disconnect 输出，此处不重复打日志、不 churn 检测。 */
}

/* ─── 4. 状态上报（等价 mqtt_publish_status）────────────────────────── */

static int local_publish_status(const struct node_config *cfg,
                                const struct device_info *dev,
                                const char *status)
{
    return mqtt_publish_status(cfg, dev, status);
}

/* ─── 5. 数据上报（等价 v1.2.11 main.c publish_by_source）────────────── */

static int local_publish_data(const struct node_config *cfg,
                              const struct sensor_data *data)
{
    if (!cfg || !data)
        return E_INVAL;

    if (data->source == SOURCE_LOCAL) {
        /* 本地传感器：默认主题，不 mask 哨兵（§3.5-4） */
        return mqtt_publish(cfg, data);
    }

    /* Modbus：topic/modbus，JSON，QoS1；复用纯函数构造（截断拒绝） */
    char modbus_topic[256];
    snprintf(modbus_topic, sizeof(modbus_topic), "%s/modbus", cfg->topic);

    char payload[512];
    int rc = mqtt_build_data_payload(cfg, data, payload, (int)sizeof(payload));
    if (rc != E_OK)
        return rc;
    return mqtt_publish_raw(modbus_topic, payload, 1);
}

/* ─── 6. 告警上报（等价 v1.2.11 main.c alert 分支）───────────────────── */

static int local_publish_alert(const struct node_config *cfg,
                               const struct alert_event *evt)
{
    if (!cfg || !evt)
        return E_INVAL;

    /* msg 文本与 v1.2.11 完全相同；local 只用 msg 字段 */
    char alert_topic[256];
    snprintf(alert_topic, sizeof(alert_topic), "%s/alert", cfg->topic);
    return mqtt_publish_raw(alert_topic, evt->msg, 1);
}

/* ─── 7. 下行消息（local 唯一 ota/cmd 路由）──────────────────────────── */

static void local_on_message(const char *topic, const char *payload, int len)
{
    if (!topic || !payload)
        return;

    const struct node_config *cfg = platform_node_config();

    /* 严格等价 v1.2.11：只有 ota.enabled 时才注册过 ota 回调 → 此处按
     * ota.enabled 门控。ota 未启用/未 init 时：不进入 OTA 状态机。
     * （ota_handle_message 自身也有 !g_cfg.enabled 早返回，双重护栏。） */
    if (!cfg || !cfg->ota.enabled) {
        LOG_INFO("local: drop downstream message on '%s' (ota disabled)",
                 topic);
        return;
    }

    if (strstr(topic, "/ota/cmd")) {
        ota_handle_message(payload, len);
    } else {
        LOG_INFO("local: no route for topic '%s'", topic);
    }
}

/* ─── 8. 周期 tick（local 无维护项）────────────────────────────────── */

static void local_tick(const struct node_config *cfg)
{
    (void)cfg;
}

/* ─── 9. 关闭（local 无额外清理；mqtt_close 由 main 统一调用）────────── */

static void local_shutdown(void)
{
}

const struct platform_ops platform_local_ops = {
    .name             = "local",
    .connect_params   = local_connect_params,
    .on_connected     = local_on_connected,
    .on_disconnected  = local_on_disconnected,
    .on_message       = local_on_message,
    .publish_status   = local_publish_status,
    .publish_data     = local_publish_data,
    .publish_alert    = local_publish_alert,
    .tick             = local_tick,
    .shutdown         = local_shutdown,
};
