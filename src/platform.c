/*
 * platform.c
 * 平台适配层分发器（docs/12 §1.2 / §3.1）。
 *
 * 仅做装配与薄转发：
 *   - platform_select() 在"所有线程创建之前"由 main 调用一次，写定 g_active；
 *   - 其余 API 全部是对 g_active 的转发，运行期 g_active 只读、无锁
 *     （§8：分发器指针启动后只读）。
 *
 * huawei 实现尚未落地（T03/T04/T05）：platform="huawei" 时 fail-safe
 * 回落 local，保运行能力，并留清晰 TODO 锚点。
 */
#include "platform.h"
#include "platform_huawei.h"   /* hw_connect_params / hw_subdev_init / hw_ota_status_shim */
#include "subdev_registry.h"
#include "mqtt_client.h"       /* T05 Q3：platform_ota_status_publish → mqtt_publish_raw */
#include <string.h>

/* 选定的平台 ops。默认 local——保证未调用 platform_select 的单元测试
 * （如 mqtt_client 的 CONNACK 单测）也落在 v1.2.11 的默认行为上。 */
static const struct platform_ops *g_active = &platform_local_ops;

/* 只读上下文：初始化期（线程创建前）注入，运行期只读。 */
static const struct node_config *g_cfg = NULL;
static const struct device_info *g_dev = NULL;
static void (*g_reboot_cb)(void) = NULL;

int platform_select(struct node_config *cfg)
{
    if (!cfg)
        return E_INVAL;

    g_cfg = cfg;

    /* platform 缺省（空串）按 local 处理（docs/12 §1.1 决策①） */
    const char *want = (cfg->platform[0] != '\0') ? cfg->platform : "local";

    if (strcmp(want, "local") == 0) {
        g_active = &platform_local_ops;
    } else if (strcmp(want, "huawei") == 0) {
        /* Q2 裁定：两条互斥规则——
         *  ① 凭据缺失/非法（hw_connect_params != E_OK）→ 硬回落 local（连不上华为）；
         *  ② 注册表 0 条 → 不回落（合法「纯网关」模式，仅醒目 WARN）。
         * 先校验凭据（避免在不可用配置上加载注册表），再装配运行期状态。 */
        struct platform_connect_params tmp;
        if (hw_connect_params(cfg, &tmp) != E_OK) {
            LOG_ERROR("platform='huawei' but credentials invalid "
                      "(huawei_device_id/huawei_secret empty or bad) — "
                      "falling back to local (cannot connect to IoTDA)");
            g_active = &platform_local_ops;
        } else {
            int n = hw_subdev_init(cfg);
            if (n < 0)
                LOG_WARN("platform='huawei': subdevice init returned %d", n);
            LOG_INFO("platform selected: huawei (subdevice slots=%d)", n < 0 ? 0 : n);
            g_active = &platform_huawei_ops;
        }
    } else {
        LOG_WARN("platform='%s' unknown, falling back to local", want);
        g_active = &platform_local_ops;
    }

    LOG_INFO("platform selected: %s", g_active->name);
    return E_OK;
}

const struct node_config *platform_node_config(void)
{
    return g_cfg;
}

void platform_set_device_info(const struct device_info *dev)
{
    g_dev = dev;
}

const struct device_info *platform_device_info(void)
{
    return g_dev;
}

void platform_set_reboot_request(void (*cb)(void))
{
    g_reboot_cb = cb;
}

void (*platform_reboot_request(void))(void)
{
    return g_reboot_cb;
}

/* ─── 薄转发（判空保护，ops 成员允许为空实现）────────────────── */

int platform_connect_params(const struct node_config *cfg,
                            struct platform_connect_params *out)
{
    if (!g_active || !g_active->connect_params)
        return E_INVAL;
    return g_active->connect_params(cfg, out);
}

int platform_publish_data(const struct node_config *cfg,
                          const struct sensor_data *data)
{
    if (!g_active || !g_active->publish_data)
        return E_INVAL;
    return g_active->publish_data(cfg, data);
}

int platform_publish_status(const struct node_config *cfg,
                            const struct device_info *dev,
                            const char *status)
{
    if (!g_active || !g_active->publish_status)
        return E_INVAL;
    return g_active->publish_status(cfg, dev, status);
}

int platform_publish_alert(const struct node_config *cfg,
                           const struct alert_event *evt)
{
    if (!g_active || !g_active->publish_alert)
        return E_INVAL;
    return g_active->publish_alert(cfg, evt);
}

/*
 * T05 Q3：OTA 状态发布注入点。
 * huawei 激活 → hw_ota_status_shim（OTA 状态 JSON → ota_status 事件）；
 * 其它平台（含 local）→ mqtt_publish_raw（与 v1.2.11 注入 raw 逐字节等价）。
 * 不新增 ops 成员（9 成员签名冻结）：此处按 g_active 判定，装配收敛在平台层。
 */
int platform_ota_status_publish(const char *topic, const char *payload, int qos)
{
    if (g_active == &platform_huawei_ops)
        return hw_ota_status_shim(topic, payload, qos);
    return mqtt_publish_raw(topic, payload, qos);
}

void platform_dispatch_message(const char *topic, const char *payload, int len)
{
    if (g_active && g_active->on_message)
        g_active->on_message(topic, payload, len);
}

void platform_on_connected(const struct node_config *cfg)
{
    if (g_active && g_active->on_connected)
        (void)g_active->on_connected(cfg);
}

void platform_on_disconnected(void)
{
    if (g_active && g_active->on_disconnected)
        g_active->on_disconnected();
}

void platform_tick(const struct node_config *cfg)
{
    if (g_active && g_active->tick)
        g_active->tick(cfg);
}
