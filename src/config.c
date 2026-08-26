/*
 * config.c
 * 配置文件解析模块实现
 * 支持 INI 风格键值对，格式：key = value
 * 覆盖 MQTT / TLS / Modbus / 规则引擎 / OTA / 异常检测 六大配置段
 */
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
    strncpy(cfg->sensor_i2c_dev, "/dev/i2c-1", sizeof(cfg->sensor_i2c_dev) - 1);
    cfg -> debug_level = 0;

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

    /* 规则引擎默认：无规则 */
    cfg->rule_count = 0;

    /* OTA 默认：关闭 */
    cfg->ota.enabled = 0;
    strncpy(cfg->ota.slot_dir, OTA_SLOT_DIR_DEFAULT, sizeof(cfg->ota.slot_dir) - 1);
    cfg->ota.boot_attempt_max = OTA_BOOT_ATTEMPT_MAX;

    /* 异常检测引擎默认：关闭 */
    cfg->anomaly_enabled = 0;
    cfg->anomaly_count = 0;
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
        else if (strcmp(k, "sensor_i2c_dev") == 0)
            strncpy(cfg->sensor_i2c_dev, v, sizeof(cfg->sensor_i2c_dev) - 1);
        else if (strcmp(k,"debug_level") == 0)
            cfg->debug_level = atoi(v);

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

        /* ── 规则引擎: rule_N = field,operator,threshold,action ── */
        else if (strncmp(k, "rule_", 5) == 0) {
            int idx = cfg->rule_count;
            if (idx >= RULE_MAX) {
                LOG_WARN("config: too many rules, max=%d", RULE_MAX);
                continue;
            }

            struct rule *r = &cfg->rules[idx];
            memset(r, 0, sizeof(*r));

            /* 规则名称 = key 本身 (e.g. "rule_1") */
            strncpy(r->name, k, RULE_NAME_LEN - 1);
            r->name[RULE_NAME_LEN - 1] = '\0';

            /* 解析: field,operator,threshold[,action] */
            char field[32]    = {0};
            char op_str[16]   = {0};
            char th_str[32]   = {0};
            char act_str[64]  = {0};

            int matched = sscanf(v, "%31[^,],%15[^,],%31[^,],%63[^\n]",
                                 field, op_str, th_str, act_str);
            if (matched < 3) {
                LOG_WARN("config: invalid %s format, need at least field,op,th", k);
                continue;
            }

            /* 字段名 */
            strncpy(r->field, field, sizeof(r->field));
            r->field[sizeof(r->field) - 1] = '\0';

            /* 运算符 */
            if (strcmp(op_str, "gt") == 0)
                r->op = OP_GT;
            else if (strcmp(op_str, "lt") == 0)
                r->op = OP_LT;
            else if (strcmp(op_str, "eq") == 0)
                r->op = OP_EQ;
            else if (strcmp(op_str, "ne") == 0)
                r->op = OP_NE;
            else if (strcmp(op_str, "between") == 0)
                r->op = OP_BETWEEN;
            else if (strcmp(op_str, "rate") == 0)
                r->op = OP_RATE;
            else {
                LOG_WARN("config: %s unknown operator '%s'", k, op_str);
                continue;
            }

            /* 阈值 */
            if (r->op == OP_BETWEEN) {
                /* 格式: lo_hi, e.g. 950.0_1050.0 */
                if (sscanf(th_str, "%lf_%lf",
                           &r->threshold_lo, &r->threshold_hi) != 2) {
                    LOG_WARN("config: %s invalid between range '%s'", k, th_str);
                    continue;
                }
            } else if (r->op == OP_RATE) {
                /* 格式: rate_window, e.g. 5.0_60s or 5.0_60 */
                char unit[4] = {0};
                if (sscanf(th_str, "%lf_%lf%3s",
                           &r->threshold, &r->rate_window_s, unit) >= 2) {
                    /* window parsed; unit is optional and ignored */
                } else {
                    LOG_WARN("config: %s invalid rate '%s'", k, th_str);
                    continue;
                }
            } else {
                r->threshold = atof(th_str);
            }

            /* 动作（可选，默认 log_only）*/
            if (matched >= 4) {
                /* 解析逗号或加号分隔的动作列表 */
                char *saveptr = NULL;
                char *token = strtok_r(act_str, ",+", &saveptr);
                while (token) {
                    /* trim token */
                    while (*token == ' ' || *token == '\t') token++;
                    char *end = token + strlen(token) - 1;
                    while (end > token && (*end == ' ' || *end == '\t')) end--;
                    *(end + 1) = '\0';

                    if (strcmp(token, "all") == 0)
                        r->action_mask |= (ACTION_LOG_ONLY | ACTION_ALERT_MQTT
                                           | ACTION_GPIO_1 | ACTION_GPIO_2);
                    else if (strcmp(token, "alert_mqtt") == 0)
                        r->action_mask |= ACTION_ALERT_MQTT;
                    else if (strcmp(token, "gpio_1") == 0)
                        r->action_mask |= ACTION_GPIO_1;
                    else if (strcmp(token, "gpio_2") == 0)
                        r->action_mask |= ACTION_GPIO_2;
                    else if (strcmp(token, "log_only") == 0)
                        r->action_mask |= ACTION_LOG_ONLY;
                    else
                        LOG_WARN("config: %s unknown action '%s'", k, token);

                    token = strtok_r(NULL, ",+", &saveptr);
                }
            }
            if (r->action_mask == 0)
                r->action_mask = ACTION_LOG_ONLY;  /* 默认 */

            /* 默认冷却时间 60s */
            r->cooldown_ms = 60000;

            cfg->rule_count++;
            LOG_INFO("config: %s field=%s op=%d th=%.2f act=0x%02x",
                     r->name, r->field, r->op,
                     r->threshold, r->action_mask);
        }

        /* ── OTA: ota_* 配置项 ── */
        else if (strcmp(k, "ota_enabled") == 0)
            cfg->ota.enabled = atoi(v);
        else if (strcmp(k, "ota_slot_dir") == 0)
            strncpy(cfg->ota.slot_dir, v, sizeof(cfg->ota.slot_dir) - 1);
        else if (strcmp(k, "ota_boot_attempt_max") == 0)
            cfg->ota.boot_attempt_max = atoi(v);

        /* ── 异常检测引擎: anomaly_enabled / anomaly_N ── */
        else if (strcmp(k, "anomaly_enabled") == 0)
            cfg->anomaly_enabled = atoi(v);

        else if (strncmp(k, "anomaly_", 8) == 0) {
            int idx = cfg->anomaly_count;
            if (idx >= ANOMALY_MAX) {
                LOG_WARN("config: too many anomaly rules, max=%d",
                         ANOMALY_MAX);
                continue;
            }

            struct anomaly_config *a = &cfg->anoms[idx];
            memset(a, 0, sizeof(*a));

            /* 名称 = key 本身 (e.g. "anomaly_1") */
            strncpy(a->name, k, ANOMALY_NAME_LEN - 1);
            a->name[ANOMALY_NAME_LEN - 1] = '\0';

            /* 解析: field,algo,threshold,action */
            char field[32]    = {0};
            char algo_str[16] = {0};
            char th_str[32]   = {0};
            char act_str[64]  = {0};

            int matched = sscanf(v, "%31[^,],%15[^,],%31[^,],%63[^\n]",
                                 field, algo_str, th_str, act_str);
            if (matched < 3) {
                LOG_WARN("config: invalid %s format, "
                         "need field,algo,threshold", k);
                continue;
            }

            /* 字段名 */
            strncpy(a->field, field, sizeof(a->field) - 1);
            a->field[sizeof(a->field) - 1] = '\0';

            /* 算法 */
            if (strcmp(algo_str, "zscore") == 0)
                a->algo = ANOMALY_ZSCORE;
            else if (strcmp(algo_str, "iforest") == 0) {
                a->algo = ANOMALY_IFOREST;
                a->iforest_enabled = 1;
            } else {
                LOG_WARN("config: %s unknown algo '%s'", k, algo_str);
                continue;
            }

            /* 阈值 */
            a->zscore_threshold = atof(th_str);

            /* 动作（可选，默认 log_only） */
            if (matched >= 4) {
                char *saveptr = NULL;
                char *token = strtok_r(act_str, ",+", &saveptr);
                while (token) {
                    while (*token == ' ' || *token == '\t') token++;
                    char *end = token + strlen(token) - 1;
                    while (end > token && (*end == ' ' || *end == '\t'))
                        end--;
                    *(end + 1) = '\0';

                    if (strcmp(token, "all") == 0)
                        a->action_mask |= (ACTION_LOG_ONLY | ACTION_ALERT_MQTT
                                          | ACTION_GPIO_1 | ACTION_GPIO_2);
                    else if (strcmp(token, "alert_mqtt") == 0)
                        a->action_mask |= ACTION_ALERT_MQTT;
                    else if (strcmp(token, "gpio_1") == 0)
                        a->action_mask |= ACTION_GPIO_1;
                    else if (strcmp(token, "gpio_2") == 0)
                        a->action_mask |= ACTION_GPIO_2;
                    else if (strcmp(token, "log_only") == 0)
                        a->action_mask |= ACTION_LOG_ONLY;
                    else
                        LOG_WARN("config: %s unknown action '%s'", k, token);

                    token = strtok_r(NULL, ",+", &saveptr);
                }
            }
            if (a->action_mask == 0)
                a->action_mask = ACTION_LOG_ONLY;

            /* 默认参数 */
            a->cooldown_ms = 60000;
            a->window_size = ANOMALY_WINDOW_SIZE;

            cfg->anomaly_count++;
            LOG_INFO("config: %s field=%s algo=%s th=%.2f act=0x%02x",
                     a->name, a->field,
                     a->algo == ANOMALY_ZSCORE ? "zscore" : "iforest",
                     a->zscore_threshold, a->action_mask);
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
    LOG_INFO("sensor_i2c_dev     = %s", cfg->sensor_i2c_dev);
    LOG_INFO("debug_level        = %d", cfg->debug_level);

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

    LOG_INFO("--- Rules ---");
    LOG_INFO("rule_count         = %d", cfg->rule_count);
    for (int i = 0; i < cfg->rule_count; i++) {
        const struct rule *r = &cfg->rules[i];
        const char *op_name = "?";
        switch (r->op) {
        case OP_GT:      op_name = "gt";      break;
        case OP_LT:      op_name = "lt";      break;
        case OP_EQ:      op_name = "eq";      break;
        case OP_NE:      op_name = "ne";      break;
        case OP_BETWEEN: op_name = "between"; break;
        case OP_RATE:    op_name = "rate";    break;
        }
        if (r->op == OP_BETWEEN) {
            LOG_INFO("  %s: %s %s [%.2f,%.2f] act=0x%02x",
                     r->name, r->field, op_name,
                     r->threshold_lo, r->threshold_hi,
                     r->action_mask);
        } else if (r->op == OP_RATE) {
            LOG_INFO("  %s: %s %s %.2f/%.0fs act=0x%02x",
                     r->name, r->field, op_name,
                     r->threshold, r->rate_window_s,
                     r->action_mask);
        } else {
            LOG_INFO("  %s: %s %s %.2f act=0x%02x",
                     r->name, r->field, op_name,
                     r->threshold, r->action_mask);
        }
    }

    LOG_INFO("--- OTA ---");
    LOG_INFO("ota_enabled        = %d", cfg->ota.enabled);
    LOG_INFO("ota_slot_dir       = %s", cfg->ota.slot_dir);
    LOG_INFO("ota_boot_attempt_max= %d", cfg->ota.boot_attempt_max);

    LOG_INFO("--- Anomaly Engine ---");
    LOG_INFO("anomaly_enabled    = %d", cfg->anomaly_enabled);
    LOG_INFO("anomaly_count      = %d", cfg->anomaly_count);
    for (int i = 0; i < cfg->anomaly_count; i++) {
        const struct anomaly_config *a = &cfg->anoms[i];
        const char *algo_name =
            (a->algo == ANOMALY_ZSCORE)  ? "zscore" :
            (a->algo == ANOMALY_IFOREST) ? "iforest" : "?";
        LOG_INFO("  %s: %s %s th=%.2f act=0x%02x cd=%dms win=%d",
                 a->name, a->field, algo_name,
                 a->zscore_threshold, a->action_mask,
                 a->cooldown_ms, a->window_size);
    }
}