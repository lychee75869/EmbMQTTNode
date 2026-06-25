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

    /* Modbus 默认：关闭，TCP 模式 */
    cfg->modbus.enabled = 0;
    strncpy(cfg->modbus.mode, "tcp", sizeof(cfg->modbus.mode) - 1);
    strncpy(cfg->modbus.tcp_host, "127.0.0.1", sizeof(cfg->modbus.tcp_host) - 1);
    cfg->modbus.tcp_port = 502;
    strncpy(cfg->modbus.serial_port, "/dev/ttyUSB0", sizeof(cfg->modbus.serial_port) - 1);
    cfg->modbus.baudrate = 9600;
    cfg->modbus.parity[0] = 'N';
    cfg->modbus.parity[1] = '\0';
    cfg->modbus.data_bits = 8;
    cfg->modbus.stop_bits = 1;
    cfg->modbus.poll_interval_ms = 2000;
    cfg->modbus.reg_count = 0;
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

        /* ── Modbus ── */
        else if (strcmp(k, "modbus_enabled") == 0)
            cfg->modbus.enabled = atoi(v);
        else if (strcmp(k, "modbus_mode") == 0)
            strncpy(cfg->modbus.mode, v, sizeof(cfg->modbus.mode) - 1);
        else if (strcmp(k, "modbus_serial_port") == 0)
            strncpy(cfg->modbus.serial_port, v, sizeof(cfg->modbus.serial_port) - 1);
        else if (strcmp(k, "modbus_baudrate") == 0)
            cfg->modbus.baudrate = atoi(v);
        else if (strcmp(k, "modbus_parity") == 0)
            strncpy(cfg->modbus.parity, v, sizeof(cfg->modbus.parity) - 1);
        else if (strcmp(k, "modbus_data_bits") == 0)
            cfg->modbus.data_bits = atoi(v);
        else if (strcmp(k, "modbus_stop_bits") == 0)
            cfg->modbus.stop_bits = atoi(v);
        else if (strcmp(k, "modbus_tcp_host") == 0)
            strncpy(cfg->modbus.tcp_host, v, sizeof(cfg->modbus.tcp_host) - 1);
        else if (strcmp(k, "modbus_tcp_port") == 0)
            cfg->modbus.tcp_port = atoi(v);
        else if (strcmp(k, "modbus_poll_interval_ms") == 0)
            cfg->modbus.poll_interval_ms = atoi(v);

        /* Modbus 寄存器映射: modbus_reg_N = id,addr,count,func,type,field,scale,offset */
        else if (strncmp(k, "modbus_reg_", 11) == 0) {
            int idx = cfg->modbus.reg_count;
            if (idx >= MODBUS_REG_MAX) {
                LOG_WARN("config: too many modbus reg maps, max=%d", MODBUS_REG_MAX);
                continue;
            }
            struct modbus_reg_map *reg = &cfg->modbus.regs[idx];
            memset(reg, 0, sizeof(*reg));
            int matched = sscanf(v, "%d,%d,%d,%d,%15[^,],%31[^,],%lf,%lf",
                                 &reg->slave_id,
                                 &reg->reg_addr,
                                 &reg->reg_count,
                                 &reg->func_code,
                                 reg->data_type,
                                 reg->field_name,
                                 &reg->scale,
                                 &reg->offset);
            if (matched >= 6) {
                cfg->modbus.reg_count++;
                LOG_INFO("config: modbus reg[%d] slave=%d addr=%d "
                         "type=%s field=%s scale=%.3f offset=%.3f",
                         idx, reg->slave_id, reg->reg_addr,
                         reg->data_type, reg->field_name,
                         reg->scale, reg->offset);
            } else {
                LOG_WARN("config: invalid modbus_reg_%d format", idx);
            }
        }
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

    LOG_INFO("--- Modbus ---");
    LOG_INFO("modbus_enabled     = %d", cfg->modbus.enabled);
    if (cfg->modbus.enabled) {
        LOG_INFO("modbus_mode        = %s", cfg->modbus.mode);
        if (strcmp(cfg->modbus.mode, "rtu") == 0)
            LOG_INFO("modbus_port        = %s %d %c%d%c",
                     cfg->modbus.serial_port, cfg->modbus.baudrate,
                     cfg->modbus.parity[0],
                     cfg->modbus.data_bits, cfg->modbus.stop_bits);
        else
            LOG_INFO("modbus_host        = %s:%d",
                     cfg->modbus.tcp_host, cfg->modbus.tcp_port);
        LOG_INFO("modbus_poll_ms     = %d", cfg->modbus.poll_interval_ms);
        LOG_INFO("modbus_reg_count   = %d", cfg->modbus.reg_count);
        for (int i = 0; i < cfg->modbus.reg_count; i++) {
            const struct modbus_reg_map *r = &cfg->modbus.regs[i];
            LOG_INFO("  reg[%d]: slave=%d addr=%d count=%d "
                     "func=%d type=%s field=%s scale=%.3f offset=%.3f",
                     i, r->slave_id, r->reg_addr, r->reg_count,
                     r->func_code, r->data_type, r->field_name,
                     r->scale, r->offset);
        }
    }
}