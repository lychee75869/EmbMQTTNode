#include "mqtt_client.h"
#include <mosquitto.h>
#include <stdio.h>

static struct mosquitto *g_mosq = NULL;
static volatile int g_connected = 0;

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq; (void)obj;
    if (rc == 0) {
        g_connected = 1;
        LOG_INFO("mqtt connected");
    } else {
        g_connected = 0;
        LOG_ERROR("mqtt connect failed, code: %d", rc);
    }
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq; (void)obj;
    g_connected = 0;
    LOG_INFO("mqtt disconnected, code: %d", rc);
}

int mqtt_init(const char *host, int port, const char *client_id)
{
    mosquitto_lib_init();

    g_mosq = mosquitto_new(client_id, true, NULL);
    if (!g_mosq) {
        LOG_ERROR("mosquitto_new failed");
        return E_NET;
    }

    mosquitto_connect_callback_set(g_mosq, on_connect);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect);

    int rc = mosquitto_connect(g_mosq, host, port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_connect failed: %s", mosquitto_strerror(rc));
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
        mosquitto_lib_cleanup();
        return E_NET;
    }

    /* 启动后台网络线程 */
    rc = mosquitto_loop_start(g_mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_loop_start failed: %s", mosquitto_strerror(rc));
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
        mosquitto_lib_cleanup();
        return E_NET;
    }

    return E_OK;
}

int mqtt_publish(const struct node_config *cfg, const struct sensor_data *data)
{
    if (!g_mosq || !g_connected || !cfg || !data) return E_INVAL;

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"client_id\":\"%s\",\"timestamp\":%lld,\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f}",
             cfg->client_id,
             (long long)data->timestamp_ms,
             data->temperature,
             data->humidity,
             data->pressure);

    int rc = mosquitto_publish(g_mosq, NULL, cfg->topic,
                               (int)strlen(payload), payload, 1, 0);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERROR("mosquitto_publish failed: %s", mosquitto_strerror(rc));
        g_connected = 0;
        return E_NET;
    }

    LOG_INFO("published: %s", payload);
    return E_OK;
}

int mqtt_is_connected(void)
{
    return g_connected;
}

void mqtt_loop(int timeout_ms)
{
    (void)timeout_ms;
    /* 网络线程已在后台运行，这里可空或处理重连 */
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
}
