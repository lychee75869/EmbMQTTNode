/*
 * mqtt_client.c
 * MQTT 客户端实现，基于 libmosquitto
 * 支持 TLS 1.2+ 加密、Last Will 遗嘱消息、设备状态上报、
 * OTA 升级指令订阅
 */
#include "mqtt_client.h"
#include <mosquitto.h>
#include <stdio.h>
#include <stdatomic.h>

/* OpenSSL 常量（避免引入 libssl-dev 依赖） */
#ifndef SSL_VERIFY_PEER
#define SSL_VERIFY_PEER  1
#endif

static struct mosquitto *g_mosq = NULL;

/*
 * P1-3: 跨线程同步。
 * 这些变量在 libmosquitto 网络线程（on_connect/on_disconnect/on_message）
 * 与业务线程（publish/subscribe/主循环）之间共享。volatile 只阻止
 * 编译器缓存，不提供原子性、不做内存序约束—— torn read/write 与
 * 指令重排下的陈旧值都是真实风险。C11 _Atomic 保证每次访问都是
 * 原子操作并附带正确的同步语义。
 */
static _Atomic int g_connected = 0;
/* 回调指针：注册（业务线程）与触发（网络线程）可能并发，
 * 指针本身的读写也必须是原子的 */
static _Atomic(mqtt_ota_callback) g_ota_cb = NULL;
/* P1-13/P2-19: "连接成功"回调，由 on_connect 在每次 CONNACK 成功时调用 */
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
        /* 先置 g_connected 再调回调：回调内的 publish/subscribe
         * 依赖 g_connected==1 的前置检查（见各函数开头） */
        mqtt_connected_callback cb = atomic_load(&g_connected_cb);
        if (cb) {
            cb();
        }
        return E_OK;
    }

    atomic_store(&g_connected, 0);
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
}

static void on_message(struct mosquitto *mosq, void *obj,
                       const struct mosquitto_message *msg)
{
    (void)mosq;
    (void)obj;

    LOG_INFO("mqtt message received on topic '%s': %d bytes",
             msg->topic, msg->payloadlen);

    /* 如果是 OTA 升级指令，交给 ota 回调处理（P1-3: 原子快照后调用，
     * 避免触发瞬间回调被另一线程清除的竞态） */
    mqtt_ota_callback ota_cb = atomic_load(&g_ota_cb);
    if (ota_cb && msg->topic && msg->payload) {
        if (strstr(msg->topic, "/ota/cmd")) {
            ota_cb((const char *)msg->payload, msg->payloadlen);
        }
    }
}

/* ─── 初始化（含 TLS）────────────────────────────────────────── */

int mqtt_init(const char *host, int port,
              const char *client_id,
              const struct tls_config *tls,
              const char *will_topic,
              const char *will_payload)
{
    int rc;

    mosquitto_lib_init();

    g_mosq = mosquitto_new(client_id, true, NULL);
    if (!g_mosq) {
        LOG_ERROR("mosquitto_new failed");
        return E_NET;
    }

    /* ── Last Will 遗嘱消息 ──────────────────────────────── */
    if (will_topic && will_payload) {
        rc = mosquitto_will_set(g_mosq, will_topic,
                                (int)strlen(will_payload),
                                will_payload, 1, 1);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERROR("mosquitto_will_set failed: %s",
                      mosquitto_strerror(rc));
            goto fail;
        }
        LOG_INFO("mqtt will set: topic=%s", will_topic);
    }

    /* ── TLS 配置 ─────────────────────────────────────── */
    if (tls && tls->enabled) {
        LOG_INFO("mqtt tls mode: %s",
                 tls->enabled == 2 ? "mutual" : "server-only");

        /* 单向认证：加载 CA 证书验证 Broker */
        rc = mosquitto_tls_set(g_mosq, tls->ca_file,
                               NULL, NULL, NULL, NULL);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERROR("mosquitto_tls_set ca failed: %s (ca_file=%s)",
                      mosquitto_strerror(rc), tls->ca_file);
            goto fail;
        }

        /* 双向认证：加载客户端证书和私钥 */
        if (tls->enabled == 2) {
            rc = mosquitto_tls_set(g_mosq, tls->ca_file,
                                   NULL, /* capath */
                                   tls->cert_file,
                                   tls->key_file,
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

    /* 用户名/密码认证 */
    if (tls && tls->username[0] != '\0') {
        rc = mosquitto_username_pw_set(g_mosq,
                                       tls->username,
                                       tls->password);
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

    /* 连接 Broker */
    rc = mosquitto_connect(g_mosq, host, port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_connect to %s:%d failed: %s",
                  host, port, mosquitto_strerror(rc));
        goto fail;
    }

    /* 启动后台网络线程 */
    rc = mosquitto_loop_start(g_mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_loop_start failed: %s",
                  mosquitto_strerror(rc));
        goto fail;
    }

    LOG_INFO("mqtt init ok: %s:%d client=%s", host, port, client_id);
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

/* ─── OTA 回调注册 ───────────────────────────────────────── */

void mqtt_set_ota_callback(mqtt_ota_callback cb)
{
    atomic_store(&g_ota_cb, cb);
    LOG_INFO("mqtt ota callback %s", cb ? "registered" : "cleared");
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

    int n = snprintf(buf, (size_t)buf_len,
                     "{\"client_id\":\"%s\","
                     "\"timestamp\":%lld,"
                     "\"temperature\":%.2f,"
                     "\"humidity\":%.2f,"
                     "\"pressure\":%.2f}",
                     cfg->client_id,
                     (long long)data->timestamp_ms,
                     data->temperature,
                     data->humidity,
                     data->pressure);

    /* snprintf 返回"本应写入"的长度：n < 0 编码失败，
     * n >= buf_len 说明缓冲不足被截断——两种情况都拒绝，
     * 不发布半截 JSON。 */
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