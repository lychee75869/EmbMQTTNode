/*
 * mqtt_client.c
 * MQTT 客户端实现，基于 libmosquitto
 * 支持 TLS 1.2+ 加密、Last Will 遗嘱消息、设备状态上报、
 * OTA 升级指令订阅
 */
#include "mqtt_client.h"
#include "platform.h"        /* T02：连接参数/下行消息走平台分发器 */
#include "sensor_fields.h"   /* 字段描述表（单一事实源，docs/12 §3.4） */
#include <mosquitto.h>
#include <stdio.h>
#include <stdatomic.h>

/* OpenSSL 常量（避免引入 libssl-dev 依赖） */
#ifndef SSL_VERIFY_PEER
#define SSL_VERIFY_PEER  1
#endif

static struct mosquitto *g_mosq = NULL;

/*
 * T02：mqtt_init 时缓存 cfg 只读指针（g_cfg 生命周期 = main 的 g_cfg 静态全局）。
 * 供 on_connect（网络线程）调用 platform_on_connected(cfg) 使用；
 * 未 mqtt_init 时（纯单测）为 NULL，platform_on_connected(NULL) 走安全空路径。
 */
static const struct node_config *g_cfg = NULL;

/*
 * P1-3: 跨线程同步。
 * 这些变量在 libmosquitto 网络线程（on_connect/on_disconnect/on_message）
 * 与业务线程（publish/subscribe/主循环）之间共享。volatile 只阻止
 * 编译器缓存，不提供原子性、不做内存序约束—— torn read/write 与
 * 指令重排下的陈旧值都是真实风险。C11 _Atomic 保证每次访问都是
 * 原子操作并附带正确的同步语义。
 */
static _Atomic int g_connected = 0;
/* P1-13/P2-19: "连接成功"观测 hook，CONNACK 成功时触发（生产不注册；
 * 主要供单测覆盖重连重订语义）。生产侧"连接成功后做什么"已由
 * platform_on_connected 承担。 */
static _Atomic(mqtt_connected_callback) g_connected_cb = NULL;

/* ─── 回调 ─────────────────────────────────────────────────── */

/*
 * P1-13/P2-19: CONNACK 处理逻辑从 on_connect 抽出为独立函数。
 * why: ① 修复 P1-13——clean_session=true（mosquitto_new 第二参）
 * 下 broker 在断连时清空本客户端全部订阅，libmosquitto 的
 * loop_start 自动重连虽会再次触发 on_connect，但订阅不会自动
 * 恢复，必须在每次连接成功时通过回调重新订阅；② 抽成无副作用
 * 入口的函数后，"注册回调 → CONNACK 成功 → 回调触发"这条
 * P1-13/P2-19 的核心链路可以在无 broker 环境下单元测试。
 * 注: 本函数运行在 libmosquitto 网络线程；回调内调用
 * mosquitto_publish/subscribe 是官方允许的线程安全用法。
 */
int mqtt_handle_connack(int rc)
{
    if (rc == 0) {
        atomic_store(&g_connected, 1);
        LOG_INFO("mqtt connected");
        /* 先置 g_connected 再触发平台/回调：其内的 publish/subscribe
         * 依赖 g_connected==1 的前置检查（见各函数开头） */
        platform_on_connected(g_cfg);
        mqtt_connected_callback cb = atomic_load(&g_connected_cb);
        if (cb) {
            cb();
        }
        return E_OK;
    }

    atomic_store(&g_connected, 0);
    platform_on_disconnected();
    switch (rc) {
    case 1:  LOG_ERROR("mqtt connect refused: protocol version"); break;
    case 2:  LOG_ERROR("mqtt connect refused: identifier rejected"); break;
    case 3:  LOG_ERROR("mqtt connect refused: broker unavailable"); break;
    case 4:  LOG_ERROR("mqtt connect refused: bad username or password"); break;
    case 5:  LOG_ERROR("mqtt connect refused: not authorized"); break;
    default: LOG_ERROR("mqtt connect failed, code: %d", rc); break;
    }
    return E_NET;
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;
    /* P1-13/P2-19: 逻辑移入 mqtt_handle_connack（可单测），本回调仅转发 */
    mqtt_handle_connack(rc);
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;
    atomic_store(&g_connected, 0);
    if (rc == 0)
        LOG_INFO("mqtt disconnected (clean)");
    else
        LOG_WARN("mqtt disconnected unexpectedly, code: %d", rc);
    /* T02：通知平台层（local 无副作用；huawei 用于 churn 检测，§8） */
    platform_on_disconnected();
}

static void on_message(struct mosquitto *mosq, void *obj,
                       const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)obj;

    LOG_INFO("mqtt message received on topic '%s': %d bytes",
             msg->topic, msg->payloadlen);

    /* T02：不再判 topic、不再分发到 ota 回调——全量转发平台分发器。
     * 唯一路由：local → platform_local.on_message（ota/cmd）。
     * msg->payload 为非 NUL 结尾字节序列，故必须带 payloadlen 转发。 */
    if (msg->topic && msg->payload)
        platform_dispatch_message(msg->topic, (const char *)msg->payload,
                                  msg->payloadlen);
}

/* ─── 初始化（含 TLS）────────────────────────────────────────── */

int mqtt_init(const struct node_config *cfg)
{
    int rc;   /* 覆盖后续 mosquitto_* 返回码 */

    if (!cfg)
        return E_INVAL;

    mosquitto_lib_init();

    /* T02：连接参数经平台分发器取得（local 静态取值；huawei 现算鉴权） */
    struct platform_connect_params p;
    if (platform_connect_params(cfg, &p) != E_OK) {
        LOG_ERROR("platform_connect_params failed");
        mosquitto_lib_cleanup();
        return E_INVAL;
    }

    /* 缓存 cfg 只读指针，供 on_connect 触发 platform_on_connected(cfg) */
    g_cfg = cfg;

    g_mosq = mosquitto_new(p.client_id, true, NULL);
    if (!g_mosq) {
        LOG_ERROR("mosquitto_new failed");
        mosquitto_lib_cleanup();
        return E_NET;
    }

    /* ── Last Will 遗嘱消息（use_will && topic 非空）────────── */
    if (p.use_will && p.will_topic[0] != '\0') {
        rc = mosquitto_will_set(g_mosq, p.will_topic,
                                (int)strlen(p.will_payload),
                                p.will_payload, 1, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERROR("mosquitto_will_set failed: %s",
                      mosquitto_strerror(rc));
            goto fail;
        }
        LOG_INFO("mqtt will set: topic=%s", p.will_topic);
    }

    /* ── TLS 配置 ─────────────────────────────────────────────
     * T02：由 force_tls（huawei 恒 1）或 cfg->tls.enabled（local）
     * 驱动——修复旧落点"TLS 仅依赖用户配置 tls_enabled"（§3.1/§8）。
     * CA 锚点：force_tls 时取 p.ca_file（华为 CA），否则 cfg->tls.ca_file。 */
    if (p.force_tls || cfg->tls.enabled) {
        const char *ca = p.force_tls ? p.ca_file : cfg->tls.ca_file;
        int mutual = (!p.force_tls && cfg->tls.enabled == 2);

        LOG_INFO("mqtt tls mode: %s", mutual ? "mutual" : "server-only");

        /* 单向认证：加载 CA 证书验证 Broker */
        rc = mosquitto_tls_set(g_mosq, ca, NULL, NULL, NULL, NULL);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERROR("mosquitto_tls_set ca failed: %s (ca_file=%s)",
                      mosquitto_strerror(rc), ca);
            goto fail;
        }

        /* 双向认证：加载客户端证书和私钥（仅 local tls_enabled==2） */
        if (mutual) {
            rc = mosquitto_tls_set(g_mosq, ca,
                                   NULL, /* capath */
                                   cfg->tls.cert_file,
                                   cfg->tls.key_file,
                                   NULL  /* pw_callback */);
            if (rc != MOSQ_ERR_SUCCESS) {
                LOG_ERROR("mosquitto_tls_set mutual failed: %s",
                          mosquitto_strerror(rc));
                goto fail;
            }
        }

        /* 强制验证：不跳过主机名检查 */
        mosquitto_tls_insecure_set(g_mosq, false);

        /* TLS 选项：最低 TLS 1.2 */
        mosquitto_tls_opts_set(g_mosq, SSL_VERIFY_PEER, "tlsv1.2", NULL);

        LOG_INFO("mqtt tls configured ok");
    }

    /* 用户名/密码认证（仅当 username 非空，等价 v1.2.11 行为） */
    if (p.username[0] != '\0') {
        rc = mosquitto_username_pw_set(g_mosq, p.username, p.password);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERROR("mosquitto_username_pw_set failed: %s",
                      mosquitto_strerror(rc));
            goto fail;
        }
        LOG_INFO("mqtt username auth set");
    }

    /* 注册回调 */
    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);
    mosquitto_message_callback_set(g_mosq, on_message);

    /* 连接 Broker（keepalive 由平台参数给出：local 恒 60） */
    rc = mosquitto_connect(g_mosq, cfg->broker_host, cfg->broker_port,
                           p.keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_connect to %s:%d failed: %s",
                  cfg->broker_host, cfg->broker_port, mosquitto_strerror(rc));
        goto fail;
    }

    /* 启动后台网络线程 */
    rc = mosquitto_loop_start(g_mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_loop_start failed: %s",
                  mosquitto_strerror(rc));
        goto fail;
    }

    LOG_INFO("mqtt init ok: %s:%d client=%s", cfg->broker_host,
             cfg->broker_port, p.client_id);
    return E_OK;

fail:
    if (g_mosq) {
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
    }
    mosquitto_lib_cleanup();
    return E_NET;
}

/* ─── Last Will 遗嘱消息 ──────────────────────────────────── */

int mqtt_set_will(const char *topic, const char *payload)
{
    if (!g_mosq || !topic || !payload) return E_INVAL;

    int rc = mosquitto_will_set(g_mosq, topic,
                                (int)strlen(payload),
                                payload, 1, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_will_set failed: %s",
                  mosquitto_strerror(rc));
        return E_NET;
    }
    LOG_INFO("mqtt will set: topic=%s", topic);
    return E_OK;
}

/* ─── 发布传感器数据 ──────────────────────────────────────── */

int mqtt_publish(const struct node_config *cfg,
                 const struct sensor_data *data)
{
    if (!g_mosq || !atomic_load(&g_connected) || !cfg || !data)
        return E_INVAL;

    char payload[512];
    /* P1-9: payload 构造下沉到纯函数 mqtt_build_data_payload，
     * snprintf 截断（n < 0 || n >= buf_len）时返回 E_IO，
     * 拒绝发布被截断的半截 JSON。 */
    int build_rc = mqtt_build_data_payload(cfg, data,
                                           payload, (int)sizeof(payload));
    if (build_rc != E_OK) {
        LOG_ERROR("mqtt_build_data_payload rejected (truncated/invalid), "
                  "topic=%s", cfg->topic);
        return build_rc;
    }

    int rc = mosquitto_publish(g_mosq, NULL, cfg->topic,
                               (int)strlen(payload), payload, 1, 0);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_publish data failed: %s",
                  mosquitto_strerror(rc));
        atomic_store(&g_connected, 0);
        return E_NET;
    }

    LOG_INFO("published: %s", payload);
    return E_OK;
}

/* ─── 发布设备状态 ────────────────────────────────────────── */

int mqtt_publish_status(const struct node_config *cfg,
                        const struct device_info *dev,
                        const char *status)
{
    if (!g_mosq || !atomic_load(&g_connected) || !cfg || !dev || !status)
        return E_INVAL;

    char status_topic[256];
    /* P1-13/P2-19: 主题构造下沉到纯函数 mqtt_build_status_topic（可单测） */
    mqtt_build_status_topic(cfg->topic, status_topic, (int)sizeof(status_topic));

    char payload[512];
    /* P1-9: payload 构造下沉到纯函数 mqtt_build_status_payload，
     * 截断时返回 E_IO，拒绝发布半截 JSON。 */
    int build_rc = mqtt_build_status_payload(cfg, dev, status,
                                             payload, (int)sizeof(payload));
    if (build_rc != E_OK) {
        LOG_ERROR("mqtt_build_status_payload rejected (truncated/invalid), "
                  "status=%s", status);
        return build_rc;
    }

    int rc = mosquitto_publish(g_mosq, NULL, status_topic,
                               (int)strlen(payload), payload, 1, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_publish status failed: %s",
                  mosquitto_strerror(rc));
        return E_NET;
    }

    LOG_INFO("device status published: %s", status);
    return E_OK;
}

/* ─── 订阅 OTA 主题 ───────────────────────────────────────── */

int mqtt_subscribe_ota(const char *client_id)
{
    if (!g_mosq || !atomic_load(&g_connected) || !client_id) return E_INVAL;

    char ota_topic[256];
    /* P1-13/P2-19: 主题构造下沉到纯函数 mqtt_build_ota_topic（可单测） */
    mqtt_build_ota_topic(client_id, ota_topic, (int)sizeof(ota_topic));

    int rc = mosquitto_subscribe(g_mosq, NULL, ota_topic, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_subscribe ota failed: %s",
                  mosquitto_strerror(rc));
        return E_NET;
    }

    LOG_INFO("ota topic subscribed: %s", ota_topic);
    return E_OK;
}

/* ─── 订阅任意主题（T02：供平台实现重订/订阅命令主题）────────── */

int mqtt_subscribe_topic(const char *topic, int qos)
{
    if (!g_mosq || !atomic_load(&g_connected) || !topic) return E_INVAL;

    int rc = mosquitto_subscribe(g_mosq, NULL, topic, qos);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_subscribe '%s' failed: %s",
                  topic, mosquitto_strerror(rc));
        return E_NET;
    }

    LOG_INFO("topic subscribed: %s (qos=%d)", topic, qos);
    return E_OK;
}

/* ─── 连接成功回调注册（P1-13/P2-19）───────────────────── */

void mqtt_set_connected_callback(mqtt_connected_callback cb)
{
    atomic_store(&g_connected_cb, cb);
    LOG_INFO("mqtt connected callback %s", cb ? "registered" : "cleared");
}

/* ─── 主题构造（纯函数，P1-13/P2-19 供单元测试）────────── */

int mqtt_build_ota_topic(const char *client_id, char *buf, int buf_len)
{
    if (!client_id || !buf || buf_len <= 0) return E_INVAL;

    snprintf(buf, (size_t)buf_len, "embmqttnode/%s/ota/cmd", client_id);
    return E_OK;
}

int mqtt_build_status_topic(const char *base_topic, char *buf, int buf_len)
{
    if (!base_topic || !buf || buf_len <= 0) return E_INVAL;

    snprintf(buf, (size_t)buf_len, "%s/status", base_topic);
    return E_OK;
}

/* ─── payload 构造（纯函数，P1-9 供单元测试）────────────── */

int mqtt_build_data_payload(const struct node_config *cfg,
                            const struct sensor_data *data,
                            char *buf, int buf_len)
{
    if (!cfg || !data || !buf || buf_len <= 0) return E_INVAL;

    /* 前缀 + 逐字段追加 + 收尾。字段序 == SENSOR_FIELDS 表序 ==
     * v1.2.11 payload 序（temperature,humidity,pressure），故 local
     * 路径输出与重构前逐字节一致（见 sensor_fields.h 表序不变式）。 */
    int n = snprintf(buf, (size_t)buf_len,
                     "{\"client_id\":\"%s\",\"timestamp\":%lld",
                     cfg->client_id,
                     (long long)data->timestamp_ms);
    if (n < 0 || n >= buf_len) return E_IO;

    for_each_field(f) {
        if (n < 0 || n >= buf_len) return E_IO;
        n += snprintf(buf + n, (size_t)(buf_len - n),
                      ",\"%s\":%.2f", f->name,
                      sensor_get_field(data, f->name));
    }
    if (n < 0 || n >= buf_len) return E_IO;

    n += snprintf(buf + n, (size_t)(buf_len - n), "}");
    if (n < 0 || n >= buf_len) return E_IO;

    return E_OK;
}

int mqtt_build_status_payload(const struct node_config *cfg,
                              const struct device_info *dev,
                              const char *status,
                              char *buf, int buf_len)
{
    if (!cfg || !dev || !status || !buf || buf_len <= 0) return E_INVAL;

    int n = snprintf(buf, (size_t)buf_len,
                     "{\"client_id\":\"%s\","
                     "\"status\":\"%s\","
                     "\"version\":\"%s\","
                     "\"hostname\":\"%s\","
                     "\"mac\":\"%s\","
                     "\"cpu\":\"%s\","
                     "\"kernel\":\"%s\","
                     "\"mem_kb\":%lld,"
                     "\"timestamp\":%lld}",
                     cfg->client_id,
                     status,
                     EMBMQTTNODE_VERSION,
                     dev->hostname,
                     dev->mac_addr,
                     dev->cpu_model,
                     dev->kernel_ver,
                     (long long)dev->total_mem_kb,
                     (long long)time(NULL) * 1000LL);

    if (n < 0 || n >= buf_len) return E_IO;

    return E_OK;
}

/* ─── 原始发布（自定义 topic + payload）──────────────────── */

int mqtt_publish_raw(const char *topic, const char *payload, int qos)
{
    if (!g_mosq || !atomic_load(&g_connected) || !topic || !payload)
        return E_INVAL;

    int rc = mosquitto_publish(g_mosq, NULL, topic,
                               (int)strlen(payload), payload, qos, 0);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mqtt_publish_raw failed: %s", mosquitto_strerror(rc));
        atomic_store(&g_connected, 0);
        return E_NET;
    }
    return E_OK;
}

/* ─── 工具函数 ────────────────────────────────────────────── */

int mqtt_is_connected(void)
{
    return atomic_load(&g_connected);
}

void mqtt_loop(int timeout_ms)
{
    (void)timeout_ms;
    /* 网络线程已在后台运行 */
}

void mqtt_close(void)
{
    if (g_mosq) {
        mosquitto_loop_stop(g_mosq, true);
        mosquitto_disconnect(g_mosq);
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
    }
    mosquitto_lib_cleanup();
    atomic_store(&g_connected, 0);
    LOG_INFO("mqtt closed");
}