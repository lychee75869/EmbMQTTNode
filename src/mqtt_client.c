/*
 * mqtt_client.c
 * MQTT 客户端实现，基于 libmosquitto
 * 支持 TLS 1.2+ 加密、Last Will 遗嘱消息、设备状态上报、
 * OTA 升级指令订阅
 */
#include "mqtt_client.h"
#include <mosquitto.h>
#include <stdio.h>

/* OpenSSL 常量（避免引入 libssl-dev 依赖） */
#ifndef SSL_VERIFY_PEER
#define SSL_VERIFY_PEER  1
#endif

static struct mosquitto *g_mosq = NULL;
static volatile int g_connected = 0;
static mqtt_ota_callback g_ota_cb = NULL;

/* ─── 回调 ─────────────────────────────────────────────────── */

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;

    if (rc == 0) {
        g_connected = 1;
        LOG_INFO("mqtt connected");
    } else {
        g_connected = 0;
        switch (rc) {
        case 1:  LOG_ERROR("mqtt connect refused: protocol version"); break;
        case 2:  LOG_ERROR("mqtt connect refused: identifier rejected"); break;
        case 3:  LOG_ERROR("mqtt connect refused: broker unavailable"); break;
        case 4:  LOG_ERROR("mqtt connect refused: bad username or password"); break;
        case 5:  LOG_ERROR("mqtt connect refused: not authorized"); break;
        default: LOG_ERROR("mqtt connect failed, code: %d", rc); break;
        }
    }
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;
    g_connected = 0;
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

    /* 如果是 OTA 升级指令，交给 ota 回调处理 */
    if (g_ota_cb && msg->topic && msg->payload) {
        if (strstr(msg->topic, "/ota/cmd")) {
            g_ota_cb((const char *)msg->payload, msg->payloadlen);
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
    if (!g_mosq || !g_connected || !cfg || !data) return E_INVAL;

    char payload[512];
    snprintf(payload, sizeof(payload),
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

    int rc = mosquitto_publish(g_mosq, NULL, cfg->topic,
                               (int)strlen(payload), payload, 1, 0);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_publish data failed: %s",
                  mosquitto_strerror(rc));
        g_connected = 0;
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
    if (!g_mosq || !g_connected || !cfg || !dev || !status)
        return E_INVAL;

    char status_topic[256];
    snprintf(status_topic, sizeof(status_topic),
             "%s/status", cfg->topic);

    char payload[512];
    snprintf(payload, sizeof(payload),
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
    if (!g_mosq || !g_connected || !client_id) return E_INVAL;

    char ota_topic[256];
    snprintf(ota_topic, sizeof(ota_topic),
             "embmqttnode/%s/ota/cmd", client_id);

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
    g_ota_cb = cb;
    LOG_INFO("mqtt ota callback %s", cb ? "registered" : "cleared");
}

/* ─── 原始发布（自定义 topic + payload）──────────────────── */

int mqtt_publish_raw(const char *topic, const char *payload, int qos)
{
    if (!g_mosq || !g_connected || !topic || !payload)
        return E_INVAL;

    int rc = mosquitto_publish(g_mosq, NULL, topic,
                               (int)strlen(payload), payload, qos, 0);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mqtt_publish_raw failed: %s", mosquitto_strerror(rc));
        g_connected = 0;
        return E_NET;
    }
    return E_OK;
}

/* ─── 工具函数 ────────────────────────────────────────────── */

int mqtt_is_connected(void)
{
    return g_connected;
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
    g_connected = 0;
    LOG_INFO("mqtt closed");
}