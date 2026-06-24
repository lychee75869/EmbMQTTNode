#include "config.h"

static char *trim(char *str)
{
    char *end;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        end--;
    end[1] = '\0';
    return str;
}

static void set_default_config(struct node_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->broker_host, "127.0.0.1", sizeof(cfg->broker_host) - 1);
    cfg->broker_port = 1883;
    strncpy(cfg->topic, "embmqttnode/data", sizeof(cfg->topic) - 1);
    strncpy(cfg->client_id, "emb-node-01", sizeof(cfg->client_id) - 1);
    cfg->sample_interval_ms = 5000;
    strncpy(cfg->sensor_type, "mock", sizeof(cfg->sensor_type) - 1);

    /* TLS 默认：关闭 */
    cfg->tls.enabled = 0;
    strncpy(cfg->tls.username, "", sizeof(cfg->tls.username) - 1);
    strncpy(cfg->tls.password, "", sizeof(cfg->tls.password) - 1);
}

int config_load(const char *path, struct node_config *cfg)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("open config %s failed: %s", path, strerror(errno));
        return E_IO;
    }

    set_default_config(cfg);

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        /* 跳过段标记 [xxx]，不作为 key=value 解析 */
        if (*p == '[') continue;

        char key[64] = {0}, value[128] = {0};
        if (sscanf(p, "%63[^=]=%127[^\n]", key, value) != 2) continue;

        char *k = trim(key);
        char *v = trim(value);

        /* ── MQTT ── */
        if (strcmp(k, "broker_host") == 0)
            strncpy(cfg->broker_host, v, sizeof(cfg->broker_host) - 1);
        else if (strcmp(k, "broker_port") == 0)
            cfg->broker_port = atoi(v);
        else if (strcmp(k, "topic") == 0)
            strncpy(cfg->topic, v, sizeof(cfg->topic) - 1);
        else if (strcmp(k, "client_id") == 0)
            strncpy(cfg->client_id, v, sizeof(cfg->client_id) - 1);
        else if (strcmp(k, "sample_interval_ms") == 0)
            cfg->sample_interval_ms = atoi(v);
        else if (strcmp(k, "sensor_type") == 0)
            strncpy(cfg->sensor_type, v, sizeof(cfg->sensor_type) - 1);

        /* ── TLS ── */
        else if (strcmp(k, "tls_enabled") == 0)
            cfg->tls.enabled = atoi(v);
        else if (strcmp(k, "tls_ca_file") == 0)
            strncpy(cfg->tls.ca_file, v, sizeof(cfg->tls.ca_file) - 1);
        else if (strcmp(k, "tls_cert_file") == 0)
            strncpy(cfg->tls.cert_file, v, sizeof(cfg->tls.cert_file) - 1);
        else if (strcmp(k, "tls_key_file") == 0)
            strncpy(cfg->tls.key_file, v, sizeof(cfg->tls.key_file) - 1);
        else if (strcmp(k, "broker_username") == 0)
            strncpy(cfg->tls.username, v, sizeof(cfg->tls.username) - 1);
        else if (strcmp(k, "broker_password") == 0)
            strncpy(cfg->tls.password, v, sizeof(cfg->tls.password) - 1);
    }

    fclose(fp);
    return E_OK;
}

void config_dump(const struct node_config *cfg)
{
    LOG_INFO("===== Config =====");
    LOG_INFO("broker_host        = %s", cfg->broker_host);
    LOG_INFO("broker_port        = %d", cfg->broker_port);
    LOG_INFO("topic              = %s", cfg->topic);
    LOG_INFO("client_id          = %s", cfg->client_id);
    LOG_INFO("sample_interval_ms = %d", cfg->sample_interval_ms);
    LOG_INFO("sensor_type        = %s", cfg->sensor_type);

    LOG_INFO("--- TLS ---");
    LOG_INFO("tls_enabled        = %d", cfg->tls.enabled);
    if (cfg->tls.enabled) {
        LOG_INFO("tls_ca_file        = %s", cfg->tls.ca_file);
        LOG_INFO("tls_cert_file      = %s", cfg->tls.cert_file);
        LOG_INFO("tls_key_file       = %s", cfg->tls.key_file);
        LOG_INFO("broker_username    = %s",
                 cfg->tls.username[0] ? cfg->tls.username : "(none)");
    }
}