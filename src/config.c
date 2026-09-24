/*
 * config.c
 * 配置文件解析模块实现
 * 支持 INI 风格键值对，格式：key = value
 * 覆盖 MQTT / TLS / Modbus  / OTA 四大配置段
 */
#include "config.h"

static char *trim(char *str) {
    char *end;
    while (*str == ' ' || *str == '\t')
        str++;
    if (*str == 0)
        return str;
    end = str + strlen(str) - 1;
    while (end > str &&
           (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        end--;
    end[1] = '\0';
    return str;
}

static void set_default_config(struct node_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->broker_host, "127.0.0.1", sizeof(cfg->broker_host) - 1);
    cfg->broker_port = 1883;
    strncpy(cfg->topic, "embmqttnode/data", sizeof(cfg->topic) - 1);
    strncpy(cfg->client_id, "emb-node-01", sizeof(cfg->client_id) - 1);
    cfg->debug_level = 0;

    /* TLS 默认：关闭 */
    cfg->tls.enabled = 0;
    strncpy(cfg->tls.username, "", sizeof(cfg->tls.username) - 1);
    strncpy(cfg->tls.password, "", sizeof(cfg->tls.password) - 1);

    /* Modbus 默认：关闭，TCP 模式 */
    cfg->modbus.enabled = 0;
    strncpy(cfg->modbus.mode, "tcp", sizeof(cfg->modbus.mode) - 1);
    strncpy(cfg->modbus.tcp_host, "127.0.0.1",
            sizeof(cfg->modbus.tcp_host) - 1);
    cfg->modbus.tcp_port = 502;
    strncpy(cfg->modbus.serial_port, "/dev/ttyUSB0",
            sizeof(cfg->modbus.serial_port) - 1);
    cfg->modbus.baudrate = 9600;
    cfg->modbus.parity[0] = 'N';
    cfg->modbus.parity[1] = '\0';
    cfg->modbus.data_bits = 8;
    cfg->modbus.stop_bits = 1;
    cfg->modbus.poll_interval_ms = 2000;
    cfg->modbus.reg_count = 0;

    /* OTA 默认：关闭 */
    cfg->ota.enabled = 0;
    strncpy(cfg->ota.slot_dir, OTA_SLOT_DIR_DEFAULT,
            sizeof(cfg->ota.slot_dir) - 1);
    cfg->ota.boot_attempt_max = OTA_BOOT_ATTEMPT_MAX;
    cfg->ota.boot_confirm_sec = OTA_BOOT_CONFIRM_SEC_DEFAULT;
    /* v1.2.9：签名公钥默认不配置（空串）→ fail-closed，OTA 升级指令被
     * 直接拒绝；固件签名体系必须显式部署（ota_public_key 指向公钥 PEM）。
     * HTTPS CA 默认空 → 尝试系统 CA 常见位置，找不到拒绝 https 下载。 */
    cfg->ota.public_key[0] = '\0';
    cfg->ota.ca_file[0] = '\0';
    cfg->ota.ca_path[0] = '\0';

    /* HTTP Dashboard 默认：启用、固定端口 8080；
     * P1-6 fail-closed：reboot token 默认空串 → /api/reboot 一律
     * 403 拒绝，必须显式配置 http_reboot_token 才能远程重启 */
    cfg->http.enabled = 1;
    cfg->http.reboot_token[0] = '\0';

    /* ── 华为云 IoTDA 接入（v1.3.0，T01）──
     * platform 缺省 "local"：保持 v1.2.11 行为，华为字段全部忽略。 */
    strncpy(cfg->platform, "local", sizeof(cfg->platform) - 1);
    cfg->huawei_device_id[0] = '\0';
    cfg->huawei_secret[0] = '\0';
    cfg->huawei_auth_type = 0;
    cfg->huawei_keepalive = 120; /* 华为推荐 120s */
    cfg->huawei_props_interval = 60;
    cfg->subdev_offline_sec = 30;
    /* P1-16：默认绝对路径（与 node.conf 同目录）。原相对默认
     * "config/subdevices.conf" 在板上 systemd
     * WorkingDirectory=/var/lib/embmqttnode 下解析到不存在的路径 → subdev_load
     * 静默失败。此处逐字消费绝对路径，不做 cwd/config_dir 相对解析。 */
    strncpy(cfg->subdevices_conf, "/etc/embmqttnode/subdevices.conf",
            sizeof(cfg->subdevices_conf) - 1);
    cfg->huawei_ca_file[0] = '\0';
}

int config_load(const char *path, struct node_config *cfg) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERROR("open config %s failed: %s", path, strerror(errno));
        return E_IO;
    }

    set_default_config(cfg);
    /* v1.3.0：行/值缓冲扩容——huawei_device_id/subdevices_conf/huawei_ca_file
     * 最大 256 字符，旧的 line[256]/value[128] 会把长值静默截断。 */
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';')
            continue;

        /* 跳过段标记 [xxx]，不作为 key=value 解析 */
        if (*p == '[')
            continue;

        char key[64] = {0}, value[320] = {0};
        if (sscanf(p, "%63[^=]=%319[^\n]", key, value) != 2)
            continue;

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
        else if (strcmp(k, "debug_level") == 0)
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
            strncpy(cfg->modbus.serial_port, v,
                    sizeof(cfg->modbus.serial_port) - 1);
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

        /* Modbus 寄存器映射: modbus_reg_N =
         * id,addr,count,func,type,field,scale,offset */
        else if (strncmp(k, "modbus_reg_", 11) == 0) {
            int idx = cfg->modbus.reg_count;
            if (idx >= MODBUS_REG_MAX) {
                LOG_WARN("config: too many modbus reg maps, max=%d",
                         MODBUS_REG_MAX);
                continue;
            }
            struct modbus_reg_map *reg = &cfg->modbus.regs[idx];
            memset(reg, 0, sizeof(*reg));
            int matched = sscanf(
                v, "%d,%d,%d,%d,%15[^,],%31[^,],%lf,%lf", &reg->slave_id,
                &reg->reg_addr, &reg->reg_count, &reg->func_code,
                reg->data_type, reg->field_name, &reg->scale, &reg->offset);
            if (matched < 6) {
                LOG_WARN("config: invalid modbus_reg_%d format", idx);
                continue;
            }

            /*
             * P1-4: 输入校验（fail-closed，非法映射整条丢弃）。
             * 旧实现照单全收：reg_count=0 或负数、reg_addr 低于
             * 基址（40001/30001）的映射会让 modbus_master_poll 里
             * 的 `reg_addr - 40001` 下溢成巨大无符号偏移（读越界/
             * 随机寄存器），reg_count 超过 MODBUS_REG_MAX 会溢出
             * 32 字 reg_buf 栈缓冲。
             * 合法域（按 Modbus 惯例）:
             *   slave_id   1..247（RTU 从站地址空间）
             *   func_code  3（保持寄存器，基址 40001）或 4（输入寄存器，基址
             * 30001） reg_count  1..MODBUS_REG_MAX(32)，且 addr 起连续 count 个
             *              寄存器不越过各自地址段上限（offset < 10000）
             */
            if (reg->slave_id < 1 || reg->slave_id > 247) {
                LOG_WARN("config: modbus_reg_%d slave_id %d out of "
                         "range 1-247, entry dropped",
                         idx, reg->slave_id);
                continue;
            }
            if (reg->reg_count < 1 || reg->reg_count > MODBUS_REG_MAX) {
                LOG_WARN("config: modbus_reg_%d reg_count %d out of "
                         "range 1-%d, entry dropped",
                         idx, reg->reg_count, MODBUS_REG_MAX);
                continue;
            }
            if (reg->func_code == 3) {
                if (reg->reg_addr < 40001 ||
                    reg->reg_addr - 40001 + reg->reg_count > 10000) {
                    LOG_WARN("config: modbus_reg_%d reg_addr %d invalid "
                             "for func 3 (need 40001..%d), entry dropped",
                             idx, reg->reg_addr,
                             40001 - 1 + 10000 - reg->reg_count + 1);
                    continue;
                }
            } else if (reg->func_code == 4) {
                if (reg->reg_addr < 30001 ||
                    reg->reg_addr - 30001 + reg->reg_count > 10000) {
                    LOG_WARN("config: modbus_reg_%d reg_addr %d invalid "
                             "for func 4 (need 30001..%d), entry dropped",
                             idx, reg->reg_addr,
                             30001 - 1 + 10000 - reg->reg_count + 1);
                    continue;
                }
            } else {
                LOG_WARN("config: modbus_reg_%d func_code %d unsupported "
                         "(need 3 or 4), entry dropped",
                         idx, reg->func_code);
                continue;
            }
            cfg->modbus.reg_count++;
            LOG_INFO("config: modbus reg[%d] slave=%d addr=%d count=%d "
                     "func=%d type=%s field=%s scale=%.3f offset=%.3f",
                     idx, reg->slave_id, reg->reg_addr, reg->reg_count,
                     reg->func_code, reg->data_type, reg->field_name,
                     reg->scale, reg->offset);
        }

        /* ── OTA: ota_* 配置项 ── */
        else if (strcmp(k, "ota_enabled") == 0)
            cfg->ota.enabled = atoi(v);
        else if (strcmp(k, "ota_slot_dir") == 0)
            strncpy(cfg->ota.slot_dir, v, sizeof(cfg->ota.slot_dir) - 1);
        else if (strcmp(k, "ota_boot_attempt_max") == 0)
            cfg->ota.boot_attempt_max = atoi(v);
        else if (strcmp(k, "ota_boot_confirm_sec") == 0)
            cfg->ota.boot_confirm_sec = atoi(v);
        else if (strcmp(k, "ota_public_key") == 0)
            strncpy(cfg->ota.public_key, v, sizeof(cfg->ota.public_key) - 1);
        else if (strcmp(k, "ota_ca_file") == 0)
            strncpy(cfg->ota.ca_file, v, sizeof(cfg->ota.ca_file) - 1);
        else if (strcmp(k, "ota_ca_path") == 0)
            strncpy(cfg->ota.ca_path, v, sizeof(cfg->ota.ca_path) - 1);

        /* ── HTTP Dashboard: http_* 配置项（P1-6）── */
        else if (strcmp(k, "http_enabled") == 0)
            cfg->http.enabled = atoi(v);
        else if (strcmp(k, "http_reboot_token") == 0)
            strncpy(cfg->http.reboot_token, v,
                    sizeof(cfg->http.reboot_token) - 1);

        /* ── 华为云 IoTDA 接入（v1.3.0，T01）── */
        else if (strcmp(k, "platform") == 0) {
            if (strcmp(v, "local") == 0 || strcmp(v, "huawei") == 0) {
                strncpy(cfg->platform, v, sizeof(cfg->platform) - 1);
                cfg->platform[sizeof(cfg->platform) - 1] = '\0';
            } else {
                LOG_WARN("config: platform '%s' invalid (local|huawei), "
                         "falling back to 'local'",
                         v);
                strncpy(cfg->platform, "local", sizeof(cfg->platform) - 1);
            }
        } else if (strcmp(k, "huawei_device_id") == 0)
            strncpy(cfg->huawei_device_id, v,
                    sizeof(cfg->huawei_device_id) - 1);
        else if (strcmp(k, "huawei_secret") == 0)
            strncpy(cfg->huawei_secret, v, sizeof(cfg->huawei_secret) - 1);
        else if (strcmp(k, "huawei_auth_type") == 0) {
            int at = atoi(v);
            if (at == 0 || at == 1) {
                cfg->huawei_auth_type = at;
            } else {
                /* fail-closed：非法签名类型丢弃用缺省 + WARN */
                LOG_WARN("config: huawei_auth_type %d invalid (need 0 or 1), "
                         "using default 0",
                         at);
                cfg->huawei_auth_type = 0;
            }
        } else if (strcmp(k, "huawei_keepalive") == 0) {
            int ka = atoi(v);
            if (ka < 30 || ka > 1200) {
                int clamped = (ka < 30) ? 30 : 1200;
                LOG_WARN("config: huawei_keepalive %d out of range 30-1200, "
                         "clamped to %d",
                         ka, clamped);
                cfg->huawei_keepalive = clamped;
            } else {
                cfg->huawei_keepalive = ka;
            }
        } else if (strcmp(k, "huawei_props_interval") == 0)
            cfg->huawei_props_interval = atoi(v);
        else if (strcmp(k, "subdev_offline_sec") == 0) {
            /* P2-28：解析期钳制（与 huawei_keepalive 同风格）。
             * 0/负数/typo（atoi 解析失败得 0）若放行，离线判定
             * `now - last_seen > offline_ms` 恒真 → 5s tick 里
             * 子设备 OFFLINE/ONLINE 反复抖动刷屏。fail-safe 回退缺省 30；
             * 上限 86400（24h，与 keepalive 的钳制对称：低值下限 30）。
             * 任何情况下 0/负值不得进入运行期。 */
            int sec = atoi(v);
            if (sec < 30 || sec > 86400) {
                int clamped = (sec < 30) ? 30 : 86400;
                LOG_WARN("config: subdev_offline_sec %d out of range "
                         "30-86400, clamped to %d",
                         sec, clamped);
                cfg->subdev_offline_sec = clamped;
            } else {
                cfg->subdev_offline_sec = sec;
            }
        } else if (strcmp(k, "subdevices_conf") == 0)
            strncpy(cfg->subdevices_conf, v, sizeof(cfg->subdevices_conf) - 1);
        else if (strcmp(k, "huawei_ca_file") == 0)
            strncpy(cfg->huawei_ca_file, v, sizeof(cfg->huawei_ca_file) - 1);
    }

    fclose(fp);
    return E_OK;
}

void config_dump(const struct node_config *cfg) {
    LOG_INFO("===== Config =====");
    LOG_INFO("broker_host        = %s", cfg->broker_host);
    LOG_INFO("broker_port        = %d", cfg->broker_port);
    LOG_INFO("topic              = %s", cfg->topic);
    LOG_INFO("client_id          = %s", cfg->client_id);
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
                     cfg->modbus.parity[0], cfg->modbus.data_bits,
                     cfg->modbus.stop_bits);
        else
            LOG_INFO("modbus_host        = %s:%d", cfg->modbus.tcp_host,
                     cfg->modbus.tcp_port);
        LOG_INFO("modbus_poll_ms     = %d", cfg->modbus.poll_interval_ms);
        LOG_INFO("modbus_reg_count   = %d", cfg->modbus.reg_count);
        for (int i = 0; i < cfg->modbus.reg_count; i++) {
            const struct modbus_reg_map *r = &cfg->modbus.regs[i];
            LOG_INFO("  reg[%d]: slave=%d addr=%d count=%d "
                     "func=%d type=%s field=%s scale=%.3f offset=%.3f",
                     i, r->slave_id, r->reg_addr, r->reg_count, r->func_code,
                     r->data_type, r->field_name, r->scale, r->offset);
        }
    }

    LOG_INFO("--- OTA ---");
    LOG_INFO("ota_enabled        = %d", cfg->ota.enabled);
    LOG_INFO("ota_slot_dir       = %s", cfg->ota.slot_dir);
    LOG_INFO("ota_boot_attempt_max= %d", cfg->ota.boot_attempt_max);
    LOG_INFO("ota_boot_confirm_sec= %d", cfg->ota.boot_confirm_sec);
    LOG_INFO("ota_public_key      = %s",
             cfg->ota.public_key[0] ? cfg->ota.public_key
                                    : "(unset, OTA commands rejected)");
    LOG_INFO("ota_ca_file         = %s",
             cfg->ota.ca_file[0] ? cfg->ota.ca_file : "(system default)");
    LOG_INFO("ota_ca_path         = %s",
             cfg->ota.ca_path[0] ? cfg->ota.ca_path : "(none)");

    LOG_INFO("--- HTTP Dashboard ---");
    LOG_INFO("http_enabled        = %d", cfg->http.enabled);
    LOG_INFO("http_reboot_token   = %s", cfg->http.reboot_token[0]
                                             ? "(configured)"
                                             : "(unset, /api/reboot rejected)");

    LOG_INFO("--- Huawei IoTDA ---");
    LOG_INFO("platform           = %s", cfg->platform);
    if (strcmp(cfg->platform, "huawei") == 0) {
        LOG_INFO("huawei_device_id   = %s",
                 cfg->huawei_device_id[0] ? cfg->huawei_device_id : "(unset)");
        /* 安全：secret 永不打明文，仅显示是否已配置 */
        LOG_INFO("huawei_secret      = %s",
                 cfg->huawei_secret[0] ? "(configured)" : "(unset)");
        LOG_INFO("huawei_auth_type   = %d", cfg->huawei_auth_type);
        LOG_INFO("huawei_keepalive   = %d", cfg->huawei_keepalive);
        LOG_INFO("huawei_props_interval = %d", cfg->huawei_props_interval);
        LOG_INFO("subdev_offline_sec = %d", cfg->subdev_offline_sec);
        LOG_INFO("subdevices_conf    = %s", cfg->subdevices_conf);
        LOG_INFO("huawei_ca_file     = %s",
                 cfg->huawei_ca_file[0] ? cfg->huawei_ca_file : "(unset)");
    }
}