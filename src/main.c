/*
 * main.c
 * 程序入口：初始化、启动采集线程、启动上报线程
 * TLS 安全连接、设备身份、MQTT 遗嘱、启动状态上报
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <errno.h>

#include "anomaly_engine.h"
#include "common.h"
#include "config.h"
#include "gpio_hal.h"
#include "http_server.h"
#include "mac_addr.h"        /* v1.2.5 P1-5：MAC 扫描逻辑独立为可测试模块 */
#include "modbus_master.h"
#include "mqtt_client.h"
#include "ota.h"
#include "platform.h"        /* T02：平台分发器（publish/告警/下行均经此） */
#include "rule_engine.h"
#include "sensor.h"
#include "storage.h"

static volatile int g_running = 1;
static struct node_config g_cfg;
static struct device_info g_dev;

/* ─── 信号处理 ──────────────────────────────────────────── */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ─── 设备身份采集 ──────────────────────────────────────── */

/*
 * 读取网卡 MAC 地址
 * 优先物理网卡 eth0 → wlan0 → wlp2s0 → enp0s3 → enp1s0；
 * 全部失败时以 lo 兜底（lo 仅作为最后一个候选）；
 * 仍失败则生成基于 PID 的伪 MAC。
 *
 * v1.2.5（P1-5 修复）：sysfs 扫描逻辑下沉到 mac_addr.c 的 mac_scan()，
 * 本函数退化为薄封装。旧实现把 "lo" 兜底条件硬编码为下标判断
 * （数组曾 4 项时 lo 在末位，扩到 6 项后 i == 6 永假），导致仅有
 * lo 可读的机器（虚拟机/容器）触发 double fclose（UB，glibc 下
 * 通常 double free abort）。新实现由 mac_scan 收敛 fclose，并
 * 以 ifaces[i+1] == NULL 判定"最后一个候选"，消除硬编码下标。
 */
static int get_mac_address(char *mac, int mac_len) {
    static const char *const ifaces[] = {"eth0",   "wlan0", "wlp2s0",
                                         "enp0s3", "enp1s0",
                                         "lo",     NULL};

    if (mac_scan("/sys/class/net", ifaces, mac, mac_len) == E_OK)
        return E_OK;

    /* 全部失败：生成基于 PID 的伪 MAC */
    snprintf(mac, mac_len, "00:00:%05d", (int)getpid());
    LOG_WARN("no valid mac found, using fallback: %s", mac);
    return E_OK;
}

/* 提取 MAC 后 6 位（去掉冒号），用于生成 client_id */
static void mac_to_short(const char *mac, char *out, int out_len) {
    int j = 0;
    for (int i = 0; mac[i] && j < out_len - 1; i++) {
        if (mac[i] != ':')
            out[j++] = mac[i];
    }
    out[j] = '\0';
}

/* 收集设备信息 */
static void collect_device_info(struct device_info *dev) {
    memset(dev, 0, sizeof(*dev));

    /* 主机名 */
    gethostname(dev->hostname, sizeof(dev->hostname) - 1);

    /* MAC 地址 */
    get_mac_address(dev->mac_addr, sizeof(dev->mac_addr));
    mac_to_short(dev->mac_addr, dev->mac_short, sizeof(dev->mac_short));

    /* 内核版本 (uname) */
    struct utsname ubuf;
    if (uname(&ubuf) == 0) {
        int n = snprintf(dev->kernel_ver, sizeof(dev->kernel_ver), "%s %s",
                         ubuf.sysname, ubuf.release);
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
                    strncpy(dev->cpu_model, model, sizeof(dev->cpu_model) - 1);
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
             dev->hostname, dev->mac_addr, dev->cpu_model, dev->kernel_ver,
             (long long)dev->total_mem_kb);
}

/* 根据 MAC 地址自动生成 client_id */
static void auto_client_id(struct node_config *cfg,
                           const struct device_info *dev) {
    if (cfg->client_id[0] == '\0' ||
        strcmp(cfg->client_id, "emb-node-01") == 0) {
        snprintf(cfg->client_id, sizeof(cfg->client_id), "emb-node-%s",
                 dev->mac_short);
        LOG_INFO("auto client_id: %s", cfg->client_id);
    }
}

/* ─── 传感器数据处理 ────────────────────────────────────── */

/*
 * T02：数据/告警上报全部经平台分发器（main 不再直调 mqtt_* 数据路径）。
 *   - 告警：platform_publish_alert(cfg,&evt)  （local: %s/alert 原文本 QoS1）
 *   - 数据：platform_publish_data(cfg,data)   （local: local→默认主题；
 *            modbus→topic/modbus JSON QoS1；不 mask 哨兵）
 * 在线直发与断网补发（upload_thread）共用同一平台路径，topic 一致。
 */
static void process_sensor_data(const struct sensor_data *data, sensor_source_t source) {
    const char *src = (source == SOURCE_LOCAL) ? "local" : "modbus";

    /* ── Dashboard 数据更新 ── */
    http_server_update_data(data);

    /* ── 规则引擎评估（out 产出结构化告警事件，msg 文本与 v1.2.11 相同）── */
    char alert_msg[256] = {0}; // 告警消息缓冲区
    struct alert_event evt;
    memset(&evt, 0, sizeof(evt));
    uint8_t actions = rule_engine_evaluate(data, alert_msg, sizeof(alert_msg),
                                           &evt);
    if (actions) {
        LOG_INFO("%s: rule actions triggered: 0x%02x", src, actions);

        /* MQTT 告警上报（经平台；本地平台发 %s/alert 原文本） */
        if ((actions & ACTION_ALERT_MQTT) && mqtt_is_connected()) {
            platform_publish_alert(&g_cfg, &evt);
        }

        /* GPIO 输出 */
        if (actions & ACTION_GPIO_1)
            gpio_hal_set(1, 1);
        if (actions & ACTION_GPIO_2)
            gpio_hal_set(2, 1);
    }

    /* ── 异常检测引擎评估  ── */
    char anomaly_msg[256] = {0};
    struct alert_event a_evt;
    memset(&a_evt, 0, sizeof(a_evt));
    uint8_t a_actions =
        anomaly_engine_evaluate(data, anomaly_msg, sizeof(anomaly_msg), &a_evt);
    if (a_actions) {
        LOG_INFO("%s: anomaly actions triggered: 0x%02x", src, a_actions);

        if ((a_actions & ACTION_ALERT_MQTT) && mqtt_is_connected()) {
            platform_publish_alert(&g_cfg, &a_evt);
        }

        if (a_actions & ACTION_GPIO_1)
            gpio_hal_set(1, 1);
        if (a_actions & ACTION_GPIO_2)
            gpio_hal_set(2, 1);
    }

    if (mqtt_is_connected()) {
        platform_publish_data(&g_cfg, data);
    } else {
        /* MQTT 离线：按来源入队（断网续传），补发时走同一平台路径 */
        storage_save(data, source, g_cfg.client_id);
    }
}

/* ─── 采集线程 ────────────────────────────────────────── */

static void *sample_thread(void *arg) {
    (void)arg;
    struct sensor_data data;

    while (g_running) {
        int rc = sensor_read(&data);
        if (rc != E_OK) {
            LOG_ERROR("sensor_read failed: %d", rc);
            usleep(g_cfg.sample_interval_ms * 1000);
            continue;
        }

        LOG_INFO("sample: temp=%.2f hum=%.2f pres=%.2f", data.temperature,
                 data.humidity, data.pressure);

        process_sensor_data(&data, SOURCE_LOCAL);

        usleep(g_cfg.sample_interval_ms * 1000);
    }
    return NULL;
}

/* ─── Modbus 轮询线程 ──────────────────────────────────── */

static void *modbus_thread(void *arg) {
    (void)arg;
    struct sensor_data data[MODBUS_REG_MAX];

    while (g_running) {
        int n = modbus_master_poll(data, MODBUS_REG_MAX);

        for (int i = 0; i < n; i++) {
            LOG_INFO("modbus: slave data temp=%.2f hum=%.2f pres=%.2f",
                     data[i].temperature, data[i].humidity, data[i].pressure);

            process_sensor_data(&data[i], SOURCE_MODBUS);

        }

        usleep(g_cfg.modbus.poll_interval_ms * 1000);
    }
    return NULL;
}

/* ─── 上报线程（断网续传）───────────────────────────────── */

/*
 * v1.2.4（P0-4 修复）：逐条 publish + 逐条删成功条目。
 * 旧逻辑循环后无条件 storage_delete_sent(max_ts)，发布失败的条目
 * 也会被一并删除导致数据永久丢失；现在仅删除确认发布成功（E_OK）
 * 的条目，失败条目保留在库中，下轮循环自动重试。
 */
static void *upload_thread(void *arg) {
    (void)arg;
    struct sensor_data pending[16];

    while (g_running) {
        /*
         * P1-10（v1.2.9）：ota_check_and_handle 已从本线程移除，
         * 由独立 ota_thread 以 500ms 周期驱动——旧实现把 OTA 状态机
         * 挂在本 5s 上报循环里，固件下载（分钟级阻塞 recv）会把断网
         * 补发、心跳上报一并卡住。这里只保留轻量的启动健康确认。
         *
         * OTA 启动健康确认计时：稳定运行 boot_confirm_sec 后由
         * ota_confirm_boot 清零 boot_attempt（内含 enabled / count==0
         * 短路，开销可忽略） */
        ota_confirm_boot();

        if (mqtt_is_connected()) {
            int n = storage_get_pending(pending, 16);
            int sent = 0;
            for (int i = 0; i < n; i++) {
                /* 按来源补发到对应 topic（经平台路径），发布成功才按主键删除 */
                if (platform_publish_data(&g_cfg, &pending[i]) == E_OK) {
                    storage_delete_by_id(pending[i].id);
                    sent++;
                }
            }
            if (sent > 0)
                LOG_INFO("uploaded %d/%d pending records", sent, n);
        }
        sleep(5);
    }
    return NULL;
}

/* ─── OTA 状态机线程（P1-10，v1.2.9）────────────────────── */

/*
 * 独立 OTA worker：500ms 周期驱动 ota_check_and_handle。
 * 下载/验签/安装这类分钟级阻塞只影响本线程；状态机字段由
 * ota.c 内 g_state_lock 保护，上报线程不再被 OTA 阻塞拖住。
 */
static void *ota_thread(void *arg) {
    (void)arg;
    while (g_running) {
        ota_check_and_handle();
        usleep(500000);   /* 500ms */
    }
    return NULL;
}

/* ─── HTTP Dashboard 线程 ─────────────────────────────── */

struct http_thread_arg {
    int port;
    const struct device_info *dev;
    const struct node_config *cfg;
};

static void *http_thread(void *arg) {
    struct http_thread_arg *a = (struct http_thread_arg *)arg;
    http_server_start(a->port, "0.0.0.0", a->dev, a->cfg);
    return NULL;
}

/* ─── 命令行帮助 ──────────────────────────────────────── */

static void usage(const char *prog) {
    printf("EmbMQTTNode v%s - Embedded MQTT Edge Node\n", EMBMQTTNODE_VERSION);
    printf("Usage: %s [-c config]\n", prog);
    printf("  -c config   指定配置文件路径\n");
    printf("  -h          显示帮助\n");
}

/* ─── 平台重启请求回调（T02：供 huawei reboot 命令，T05 启用）───────── */

/*
 * v1.2.8（P1-13/P2-19）的"连接成功后要做的事"（重订 OTA + 重发 online）
 * 已从 main 的 mqtt_set_connected_callback 迁入平台层：
 *   mqtt_handle_connack → platform_on_connected → platform_local.on_connected
 *   （local_on_connected 内重订 OTA 主题 + 重发 online）。
 * 语义不变：每次 CONNACK 成功都会重新订阅 OTA 主题并重发 online。
 *
 * 本回调仅用于平台的"重启请求"（huawei reboot 命令）：优雅停机置
 * g_running=0，交由 systemd Restart=always 拉起（决策⑤配置生效路径）。
 * local 无命令入口，本轮注册以备 T05。
 */
static void platform_reboot_request_cb(void) {
    LOG_INFO("platform requested reboot, shutting down gracefully");
    g_running = 0;
}

/* ─── 入口 ────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *cfg_path = "config/node.conf";

    int opt;
    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        switch (opt) {
        case 'c':
            cfg_path = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return 0;
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

    /* 2.5 T02：装配平台分发器（在任何线程创建之前写定 g_active）。
     * 注入 device_info 供平台 on_connected 发 online 状态；
     * 注册"重启请求"回调（huawei reboot 命令，T05 启用）。 */
    platform_set_device_info(&g_dev);
    platform_select(&g_cfg);
    platform_set_reboot_request(platform_reboot_request_cb);

    /* 3. 初始化传感器 */
    if (sensor_init(g_cfg.sensor_type, g_cfg.sensor_i2c_dev) != E_OK) {
        fprintf(stderr, "FATAL: sensor_init failed\n");
        return 1;
    }

    /* 4. 初始化本地存储（使用绝对路径 /var/lib/embmqttnode/data.db） */
    /* systemd 已配置 WorkingDirectory=/var/lib/embmqttnode，目录通常已存在； */
    /* 首次部署若不存在则尝试单层 mkdir（不递归，避免越权创建父目录）。 */
    const char *db_dir = "/var/lib/embmqttnode";
    const char *db_path = "/var/lib/embmqttnode/data.db";
    if (mkdir(db_dir, 0755) < 0 && errno != EEXIST) {
        LOG_ERROR("mkdir %s failed (%s); 请用 -c 指定其他工作目录并手工创建，或预先创建目录",
                  db_dir, strerror(errno));
        sensor_close();
        return 1;
    }
    if (storage_init(db_path) != E_OK) {
        fprintf(stderr, "FATAL: storage_init failed\n");
        sensor_close();
        return 1;
    }

    /* 5. 初始化 MQTT（T02：连接参数与遗嘱消息由平台层组装，main 不再拼装）。
     *
     * P1-13/P2-19 的"连接成功要做的事"（重订 OTA + 重发 online）已迁入
     * 平台层：mqtt_handle_connack → platform_on_connected →
     * platform_local.on_connected。语义不变——每次 CONNACK 成功都会重订
     * OTA 主题并重发 online；且该回调在 mqtt_init 内部由 CONNACK 事件驱动，
     * 不再存在"回调晚于首个 CONNACK 注册"的窗口（原 P2-19 问题在平台层
     * 天然规避）。故 main 不再注册 mqtt_set_connected_callback。 */
    if (mqtt_init(&g_cfg) != E_OK) {
        LOG_WARN("mqtt_init failed, running in offline mode");
    }

    /* 5b. T05 Q4：启动 auth_type=1 超窗重连监督线程。
     * 仅 rebuild_on_hour==1（huawei auth_type=1）才真正创建线程；
     * local 与 auth_type=0 → no-op（零影响）。 */
    mqtt_start_supervisor();

    /* 6. 初始化 Modbus（可选模块） */
    if (modbus_master_init(&g_cfg.modbus) != E_OK) {
        LOG_WARN("modbus init failed, modbus module disabled");
    }

    /* 7. 初始化规则引擎（阶段三） */
    if (g_cfg.rule_count > 0) {
        if (rule_engine_init(&g_cfg) != E_OK) {
            LOG_WARN("rule_engine_init failed");
        }
    } else {
        LOG_INFO("no rules configured, rule engine skipped");
    }

    /* 8. 初始化异常检测引擎（方向 B） */
    if (g_cfg.anomaly_enabled && g_cfg.anomaly_count > 0) {
        if (anomaly_engine_init(&g_cfg) != E_OK) {
            LOG_WARN("anomaly_engine_init failed");
        }
    } else {
        LOG_INFO("anomaly engine disabled or no anomaly rules");
    }

    /* 9. 初始化 GPIO 告警输出 */
    gpio_hal_init();

    /* 10. 初始化 OTA 远程升级（阶段四） */
    if (g_cfg.ota.enabled) {
        ota_init(&g_cfg.ota, g_cfg.client_id, EMBMQTTNODE_VERSION);
        /* 注入 MQTT 发布回调（用于 OTA 状态上报）。
         * T05 Q3：改注入**平台分发器**的单一回调（不再直连 mqtt_publish_raw）——
         * huawei 激活时经 hw_ota_status_shim 把 OTA 状态转物模型事件发到
         * $oc/.../sys/events/report；local 时转发 mqtt_publish_raw，与
         * v1.2.11 逐字节等价。这是 main.c 中唯一保留的 OTA 状态注入点。 */
        ota_set_mqtt_publish(platform_ota_status_publish);
        /* T02：OTA 下行消息路由由平台层唯一持有（docs/12 §3.1）。
         * mqtt_client.on_message 全量转发 platform_dispatch_message，
         * local 平台 on_message 命中 /ota/cmd 后直连 ota_handle_message；
         * 故此处不再注册 mqtt_set_ota_callback。 */

        /* OTA 启动后检查：本地安全机制，不依赖 MQTT 连接。
         * 必须在 ota_init 之后调用（需要 slot_dir/配置就绪）。
         * 注：旧实现被包在步骤 5e 的 MQTT 成功分支内，断网重启时检查被跳过，
         *     形成 P0-3 缺陷；此处移到 ota_init 之后，与网络状态解耦。 */
        ota_post_boot_check();

        LOG_INFO("ota module initialized");
    } else {
        LOG_INFO("ota disabled by config");
    }

    /* 11. 注册信号 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 12. 启动工作线程 */
    LOG_INFO("EmbMQTTNode v%s starting...", EMBMQTTNODE_VERSION);

    pthread_t tid_sample, tid_upload, tid_modbus = 0, tid_http = 0, tid_ota = 0;
    pthread_create(&tid_sample, NULL, sample_thread, NULL);
    pthread_create(&tid_upload, NULL, upload_thread, NULL);
    /* Modbus 线程仅在 enabled 时启动 */
    if (g_cfg.modbus.enabled) {
        pthread_create(&tid_modbus, NULL, modbus_thread, NULL);
        LOG_INFO("modbus polling thread started");
    }
    /* P1-10: OTA 状态机独立线程（仅 enabled 时启动） */
    if (g_cfg.ota.enabled) {
        pthread_create(&tid_ota, NULL, ota_thread, NULL);
        LOG_INFO("ota worker thread started (500ms poll)");
    }
    /* HTTP Dashboard 线程（阶段五） */
    {
        struct http_thread_arg http_arg;
        http_arg.port = 8080;
        http_arg.dev = &g_dev;
        http_arg.cfg = &g_cfg;
        pthread_create(&tid_http, NULL, http_thread, &http_arg);
        LOG_INFO("http dashboard thread started on port 8080");
    }

    pthread_join(tid_sample, NULL);
    pthread_join(tid_upload, NULL);
    if (tid_modbus)
        pthread_join(tid_modbus, NULL);
    if (tid_ota)
        pthread_join(tid_ota, NULL);
    if (tid_http) {
        http_server_stop();
        pthread_join(tid_http, NULL);
    }

    /* 13. 优雅退出 */
    LOG_INFO("shutting down...");

    /* 发布离线状态（经平台层；best-effort，遗嘱消息兜底） */
    platform_publish_status(&g_cfg, &g_dev, "offline");
    usleep(200000); /* 给网络线程一点时间发出 */

    /* T05 Q4：先停监督线程（join），再关连接——避免关停与在途重建竞态 */
    mqtt_stop_supervisor();
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