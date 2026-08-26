/*
 * main.c
 * 程序入口：初始化、启动采集线程、启动上报线程
 * 阶段一：TLS 安全连接、设备身份、MQTT 遗嘱、启动状态上报
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/utsname.h>

#include "common.h"
#include "config.h"
#include "sensor.h"
#include "storage.h"
#include "mqtt_client.h"
#include "modbus_master.h"
#include "daemon.h"
#include "rule_engine.h"
#include "gpio_hal.h"
#include "ota.h"
#include "anomaly_engine.h"
#include "http_server.h"

static volatile int g_running = 1;
static struct node_config g_cfg;
static struct device_info g_dev;

/* ─── 信号处理 ──────────────────────────────────────────── */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ─── 设备身份采集 ──────────────────────────────────────── */

/*
 * 读取网卡 MAC 地址
 * 优先 eth0 → wlan0 → wlp2s0 → lo（兜底）
 */
static int get_mac_address(char *mac, int mac_len)
{
    FILE *fp;
    char path[64];
    const char *ifaces[] = { "eth0", "wlan0", "wlp2s0",
                             "enp0s3", "enp1s0", "lo", NULL };

    for (int i = 0; ifaces[i]; i++) {
        snprintf(path, sizeof(path),
                 "/sys/class/net/%s/address", ifaces[i]);
        fp = fopen(path, "r");
        if (fp) {
            if (fgets(mac, mac_len, fp)) {
                /* 去掉末尾换行符 */
                size_t len = strlen(mac);
                if (len > 0 && mac[len - 1] == '\n')
                    mac[len - 1] = '\0';
                fclose(fp);
                if (strcmp(ifaces[i], "lo") != 0 || i == 6)
                    return E_OK;
            }
            fclose(fp);
        }
    }
    /* 全部失败：生成基于 PID 的伪 MAC */
    snprintf(mac, mac_len, "00:00:%05d", (int)getpid());
    LOG_WARN("no valid mac found, using fallback: %s", mac);
    return E_OK;
}

/* 提取 MAC 后 6 位（去掉冒号），用于生成 client_id */
static void mac_to_short(const char *mac, char *out, int out_len)
{
    int j = 0;
    for (int i = 0; mac[i] && j < out_len - 1; i++) {
        if (mac[i] != ':')
            out[j++] = mac[i];
    }
    out[j] = '\0';
}

/* 收集设备信息 */
static void collect_device_info(struct device_info *dev)
{
    memset(dev, 0, sizeof(*dev));

    /* 主机名 */
    gethostname(dev->hostname, sizeof(dev->hostname) - 1);

    /* MAC 地址 */
    get_mac_address(dev->mac_addr, sizeof(dev->mac_addr));
    mac_to_short(dev->mac_addr, dev->mac_short, sizeof(dev->mac_short));

    /* 内核版本 (uname) */
    struct utsname ubuf;
    if (uname(&ubuf) == 0) {
        int n = snprintf(dev->kernel_ver, sizeof(dev->kernel_ver),
                         "%s %s", ubuf.sysname, ubuf.release);
        /* 如果截断，保证 null 结尾 */
        if (n < 0 || (size_t)n >= sizeof(dev->kernel_ver))
            dev->kernel_ver[sizeof(dev->kernel_ver) - 1] = '\0';
    }

    /* CPU 型号：读取 /proc/cpuinfo 第一行 model name */
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "model name", 10) == 0) {
                char *colon = strchr(line, ':');
                if (colon) {
                    char *model = colon + 2;
                    size_t len = strlen(model);
                    if (len > 0 && model[len - 1] == '\n')
                        model[len - 1] = '\0';
                    strncpy(dev->cpu_model, model,
                            sizeof(dev->cpu_model) - 1);
                }
                break;
            }
        }
        fclose(fp);
    }
    if (dev->cpu_model[0] == '\0')
        strncpy(dev->cpu_model, "Unknown CPU", sizeof(dev->cpu_model) - 1);

    /* 总内存：读取 /proc/meminfo 第一行 MemTotal */
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[128];
        if (fgets(line, sizeof(line), fp)) {
            long kb = 0;
            sscanf(line, "MemTotal: %ld kB", &kb);
            dev->total_mem_kb = kb;
        }
        fclose(fp);
    }

    LOG_INFO("device: host=%s mac=%s cpu=%s kernel=%s mem=%lldKB",
             dev->hostname, dev->mac_addr,
             dev->cpu_model, dev->kernel_ver,
             (long long)dev->total_mem_kb);
}

/* 根据 MAC 地址自动生成 client_id */
static void auto_client_id(struct node_config *cfg,
                           const struct device_info *dev)
{
    if (cfg->client_id[0] == '\0' ||
        strcmp(cfg->client_id, "emb-node-01") == 0) {
        snprintf(cfg->client_id, sizeof(cfg->client_id),
                 "emb-node-%s", dev->mac_short);
        LOG_INFO("auto client_id: %s", cfg->client_id);
    }
}

/* ─── 采集线程 ────────────────────────────────────────── */

static void *sample_thread(void *arg)
{
    (void)arg;
    struct sensor_data data;

    while (g_running) {
        int rc = sensor_read(&data);
        if (rc != E_OK) {
            LOG_ERROR("sensor_read failed: %d", rc);
            usleep(g_cfg.sample_interval_ms * 1000);
            continue;
        }

        LOG_INFO("sample: temp=%.2f hum=%.2f pres=%.2f",
                 data.temperature, data.humidity, data.pressure);

        /* ── Dashboard 数据更新（阶段五）── */
        http_server_update_data(&data);

        /* ── 规则引擎评估（阶段三）── */
        char alert_msg[256] = {0};// 告警消息缓冲区
        uint8_t actions = rule_engine_evaluate(&data, alert_msg,
                                               sizeof(alert_msg));
        if (actions) {
            LOG_INFO("rule actions triggered: 0x%02x", actions);

            /* MQTT 告警上报 */
            if ((actions & ACTION_ALERT_MQTT) && mqtt_is_connected()) {
                char alert_topic[256];
                snprintf(alert_topic, sizeof(alert_topic),
                         "%s/alert", g_cfg.topic);
                mqtt_publish_raw(alert_topic, alert_msg, 1);
            }

            /* GPIO 输出 */
            if (actions & ACTION_GPIO_1)
                gpio_hal_set(1, 1);
            if (actions & ACTION_GPIO_2)
                gpio_hal_set(2, 1);
        }

        /* ── 异常检测引擎评估（方向 B）── */
        {
            char anomaly_msg[256] = {0};
            uint8_t a_actions = anomaly_engine_evaluate(
                                    &data, anomaly_msg,
                                    sizeof(anomaly_msg));
            if (a_actions) {
                LOG_INFO("anomaly actions triggered: 0x%02x",
                         a_actions);

                if ((a_actions & ACTION_ALERT_MQTT) &&
                    mqtt_is_connected()) {
                    char alert_topic[256];
                    snprintf(alert_topic, sizeof(alert_topic),
                             "%s/alert", g_cfg.topic);
                    mqtt_publish_raw(alert_topic, anomaly_msg, 1);
                }

                if (a_actions & ACTION_GPIO_1)
                    gpio_hal_set(1, 1);
                if (a_actions & ACTION_GPIO_2)
                    gpio_hal_set(2, 1);
            }
        }

        if (mqtt_is_connected()) {
            mqtt_publish(&g_cfg, &data);
        } else {
            LOG_INFO("mqtt offline, save to local storage");
            storage_save(&data, g_cfg.client_id);
        }

        usleep(g_cfg.sample_interval_ms * 1000);
    }
    return NULL;
}

/* ─── Modbus 轮询线程 ──────────────────────────────────── */

static void *modbus_thread(void *arg)
{
    (void)arg;
    struct sensor_data data[MODBUS_REG_MAX];

    while (g_running) {
        int n = modbus_master_poll(data, MODBUS_REG_MAX);

        for (int i = 0; i < n; i++) {
            LOG_INFO("modbus: slave data temp=%.2f hum=%.2f pres=%.2f",
                     data[i].temperature, data[i].humidity,
                     data[i].pressure);

            /* ── Dashboard 数据更新（阶段五）── */
            http_server_update_data(&data[i]);

            /* ── 规则引擎评估（阶段三）── */
            char alert_msg[256] = {0};
            uint8_t actions = rule_engine_evaluate(&data[i], alert_msg,
                                                   sizeof(alert_msg));
            if (actions) {
                LOG_INFO("modbus rule actions triggered: 0x%02x", actions);

                if ((actions & ACTION_ALERT_MQTT) && mqtt_is_connected()) {
                    char alert_topic[256];
                    snprintf(alert_topic, sizeof(alert_topic),
                             "%s/alert", g_cfg.topic);
                    mqtt_publish_raw(alert_topic, alert_msg, 1);
                }

                if (actions & ACTION_GPIO_1)
                    gpio_hal_set(1, 1);
                if (actions & ACTION_GPIO_2)
                    gpio_hal_set(2, 1);
            }

            /* ── 异常检测引擎评估（方向 B）── */
            {
                char anomaly_msg[256] = {0};
                uint8_t a_actions = anomaly_engine_evaluate(
                                        &data[i], anomaly_msg,
                                        sizeof(anomaly_msg));
                if (a_actions) {
                    LOG_INFO("modbus anomaly actions: 0x%02x",
                             a_actions);

                    if ((a_actions & ACTION_ALERT_MQTT) &&
                        mqtt_is_connected()) {
                        char alert_topic[256];
                        snprintf(alert_topic, sizeof(alert_topic),
                                 "%s/alert", g_cfg.topic);
                        mqtt_publish_raw(alert_topic, anomaly_msg, 1);
                    }

                    if (a_actions & ACTION_GPIO_1)
                        gpio_hal_set(1, 1);
                    if (a_actions & ACTION_GPIO_2)
                        gpio_hal_set(2, 1);
                }
            }

            if (mqtt_is_connected()) {
                /* Modbus 数据发布到 topic/modbus */
                char modbus_topic[256];
                snprintf(modbus_topic, sizeof(modbus_topic),
                         "%s/modbus", g_cfg.topic);

                char payload[512];
                snprintf(payload, sizeof(payload),
                         "{\"client_id\":\"%s\","
                         "\"timestamp\":%lld,"
                         "\"temperature\":%.2f,"
                         "\"humidity\":%.2f,"
                         "\"pressure\":%.2f}",
                         g_cfg.client_id,
                         (long long)data[i].timestamp_ms,
                         data[i].temperature,
                         data[i].humidity,
                         data[i].pressure);

                mqtt_publish_raw(modbus_topic, payload, 1);
            } else {
                /* MQTT 离线，缓存到本地 */
                storage_save(&data[i], g_cfg.client_id);
            }
        }

        usleep(g_cfg.modbus.poll_interval_ms * 1000);
    }
    return NULL;
}

/* ─── 上报线程（断网续传）───────────────────────────────── */

static void *upload_thread(void *arg)
{
    (void)arg;
    struct sensor_data pending[16];

    while (g_running) {
        /* OTA 状态机驱动（阶段四） */
        if (g_cfg.ota.enabled)
            ota_check_and_handle();

        if (mqtt_is_connected()) {
            int n = storage_get_pending(pending, 16);
            if (n > 0) {
                int64_t max_ts = 0;
                for (int i = 0; i < n; i++) {
                    mqtt_publish(&g_cfg, &pending[i]);
                    if (pending[i].timestamp_ms > max_ts)
                        max_ts = pending[i].timestamp_ms;
                }
                storage_delete_sent(max_ts);
                LOG_INFO("uploaded %d pending records", n);
            }
        }
        sleep(5);
    }
    return NULL;
}

/* ─── HTTP Dashboard 线程 ─────────────────────────────── */

struct http_thread_arg {
    int port;
    const struct device_info *dev;
    const struct node_config *cfg;
};

static void *http_thread(void *arg)
{
    struct http_thread_arg *a = (struct http_thread_arg *)arg;
    http_server_start(a->port, "0.0.0.0", a->dev, a->cfg);
    return NULL;
}

/* ─── 命令行帮助 ──────────────────────────────────────── */

static void usage(const char *prog)
{
    printf("EmbMQTTNode v%s - Embedded MQTT Edge Node\n",
           EMBMQTTNODE_VERSION);
    printf("Usage: %s [-c config] [-d]\n", prog);
    printf("  -c config   指定配置文件路径\n");
    printf("  -d          以守护进程方式运行\n");
    printf("  -h          显示帮助\n");
}

/* ─── 入口 ────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *cfg_path = "config/node.conf";
    int daemon_mode = 0;

    int opt;
    while ((opt = getopt(argc, argv, "c:dh")) != -1) {
        switch (opt) {
        case 'c': cfg_path = optarg; break;
        case 'd': daemon_mode = 1; break;
        case 'h':
        default:  usage(argv[0]); return 0;
        }
    }

    /* 1. 加载配置 */
    if (config_load(cfg_path, &g_cfg) != E_OK) {
        fprintf(stderr, "FATAL: config_load failed\n");
        return 1;
    }

    /* 2. 收集设备信息 + 自动生成 client_id */
    collect_device_info(&g_dev);
    auto_client_id(&g_cfg, &g_dev);
    config_dump(&g_cfg);

    /* 3. 初始化传感器 */
    if (sensor_init(g_cfg.sensor_type, g_cfg.sensor_i2c_dev) != E_OK) {
        fprintf(stderr, "FATAL: sensor_init failed\n");
        return 1;
    }

    /* 4. 初始化本地存储 */
    if (storage_init("data.db") != E_OK) {
        fprintf(stderr, "FATAL: storage_init failed\n");
        sensor_close();
        return 1;
    }

    /* 5. 初始化 MQTT（含 TLS + 遗嘱消息） */
    /* 5a. 构造遗嘱消息 */
    char will_payload[256];
    snprintf(will_payload, sizeof(will_payload),
             "{\"client_id\":\"%s\",\"status\":\"offline\","
             "\"timestamp\":%lld}",
             g_cfg.client_id, (long long)time(NULL) * 1000LL);

    char will_topic[256];
    snprintf(will_topic, sizeof(will_topic),
             "%s/status", g_cfg.topic);

    /* 5b. MQTT 连接（传入遗嘱 topic + payload） */
    if (mqtt_init(g_cfg.broker_host,
                  g_cfg.broker_port,
                  g_cfg.client_id,
                  &g_cfg.tls,
                  will_topic,
                  will_payload) != E_OK) {
        LOG_WARN("mqtt_init failed, running in offline mode");
    } else {
        /* 5c. 等待连接建立后发布在线状态 */
        usleep(500000);
        mqtt_publish_status(&g_cfg, &g_dev, "online");

        /* 5d. 订阅 OTA 升级指令 */
        mqtt_subscribe_ota(g_cfg.client_id);

        /* 5e. OTA 启动后检查（阶段四） */
        ota_post_boot_check();
    }

    /* 6. 初始化 Modbus（可选模块） */
    if (modbus_master_init(&g_cfg.modbus) != E_OK) {
        LOG_WARN("modbus init failed, modbus module disabled");
    }

    /* 6b. 初始化规则引擎（阶段三） */
    if (g_cfg.rule_count > 0) {
        if (rule_engine_init(&g_cfg) != E_OK) {
            LOG_WARN("rule_engine_init failed");
        }
    } else {
        LOG_INFO("no rules configured, rule engine skipped");
    }

    /* 6e. 初始化异常检测引擎（方向 B） */
    if (g_cfg.anomaly_enabled && g_cfg.anomaly_count > 0) {
        if (anomaly_engine_init(&g_cfg) != E_OK) {
            LOG_WARN("anomaly_engine_init failed");
        }
    } else {
        LOG_INFO("anomaly engine disabled or no anomaly rules");
    }

    /* 6c. 初始化 GPIO 告警输出 */
    gpio_hal_init();

    /* 6d. 初始化 OTA 远程升级（阶段四） */
    if (g_cfg.ota.enabled) {
        ota_init(&g_cfg.ota, g_cfg.client_id, EMBMQTTNODE_VERSION);
        /* 注入 MQTT 发布回调（用于 OTA 状态上报） */
        ota_set_mqtt_publish(mqtt_publish_raw);
        /* 注册 OTA 消息回调（来自 MQTT 的升级指令） */
        mqtt_set_ota_callback(ota_handle_message);
        LOG_INFO("ota module initialized");
    } else {
        LOG_INFO("ota disabled by config");
    }

    /* 7. 守护进程化 */
    if (daemon_mode) {
        daemonize();
    }

    /* 8. 注册信号 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 9. 启动工作线程 */
    LOG_INFO("EmbMQTTNode v%s starting...", EMBMQTTNODE_VERSION);

    pthread_t tid_sample, tid_upload, tid_modbus = 0, tid_http = 0;
    pthread_create(&tid_sample, NULL, sample_thread, NULL);
    pthread_create(&tid_upload, NULL, upload_thread, NULL);
    /* Modbus 线程仅在 enabled 时启动 */
    if (g_cfg.modbus.enabled) {
        pthread_create(&tid_modbus, NULL, modbus_thread, NULL);
        LOG_INFO("modbus polling thread started");
    }
    /* HTTP Dashboard 线程（阶段五） */
    {
        struct http_thread_arg http_arg;
        http_arg.port = 8080;
        http_arg.dev  = &g_dev;
        http_arg.cfg  = &g_cfg;
        pthread_create(&tid_http, NULL, http_thread, &http_arg);
        LOG_INFO("http dashboard thread started on port 8080");
    }

    pthread_join(tid_sample, NULL);
    pthread_join(tid_upload, NULL);
    if (tid_modbus) pthread_join(tid_modbus, NULL);
    if (tid_http) {
        http_server_stop();
        pthread_join(tid_http, NULL);
    }

    /* 10. 优雅退出 */
    LOG_INFO("shutting down...");

    /* 发布离线状态（best-effort，遗嘱消息兜底） */
    mqtt_publish_status(&g_cfg, &g_dev, "offline");
    usleep(200000); /* 给网络线程一点时间发出 */

    mqtt_close();
    modbus_master_close();
    rule_engine_close();
    anomaly_engine_close();
    gpio_hal_close();
    ota_close();
    storage_close();
    sensor_close();

    LOG_INFO("EmbMQTTNode stopped.");
    return 0;
}