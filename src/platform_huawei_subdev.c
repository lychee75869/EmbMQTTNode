/*
 * platform_huawei_subdev.c
 * 华为云 IoTDA 子设备管理（docs/13 T04 / docs/12 §3.2/§4.2/§4.3/§8）。
 *
 * 组成：
 *   - 运行期状态 g_subdev_rt[]（Q3）：与注册表 g_entries 索引对齐，e 只读；
 *     跨线程标量用 _Atomic，全程无互斥锁（网络线程禁止锁内 I/O）。
 *   - hw_subdev_init()（Q2）：thread-before 加载注册表 + 构建对齐表。
 *   - 9 成员 platform_huawei_ops（Q4）：connect_params / on_connected /
 *     on_disconnected / on_message / publish_status / publish_data /
 *     publish_alert / tick / shutdown。
 *
 * 关键纪律：
 *   - 时钟域：last_seen_ms / next_reg_ms 一律 CLOCK_MONOTONIC 毫秒，
 *     禁止复用 data->timestamp_ms（wall-clock，NTP 步进会破坏超时判定）。
 *   - publish_data 返回契约（关键）：E_OK = 该样本已完全处置（成功发布
 *     **或** 永久不可路由而丢弃）；非 E_OK（E_NET/E_IO/E_INVAL）= 瞬态可重试。
 *     据此 main.c 续传逻辑（==E_OK → delete）零改动即正确丢弃「子设备已不存在」
 *     的陈旧记录，且不新增错误码（§8）。
 *   - on_disconnected 空实现（非阻塞-12：规避 CONNACK 失败 + 真实断连双触发虚高）。
 *   - on_message 的 CMD_REQUEST 分支仅留 TODO(T05) seam，本轮不执行命令。
 */
#include "platform.h"
#include "platform_huawei.h"
#include "mqtt_client.h"
#include "subdev_registry.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>

#define HW_THROTTLE_N 100   /* 节流 WARN：每 N 次打一条 */

/* ─── 单调时钟（毫秒）────────────────────────────────────────── */

static int64_t mono_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

/* ─── 可注入发布后端（默认 mqtt_publish_raw；供单测桩）────────── */

static hw_pub_fn g_pub = NULL;

void hw_subdev_set_publisher(hw_pub_fn fn)
{
    g_pub = fn;
}

static int pub_raw(const char *topic, const char *payload, int qos)
{
    if (g_pub)
        return g_pub(topic, payload, qos);
    return mqtt_publish_raw(topic, payload, qos);
}

/* ─── 运行期状态（Q3）────────────────────────────────────────── */

struct subdev_runtime {
    const struct subdev_entry *e;      /* 指向注册表条目（只读，加载后不变） */
    _Atomic int     registered;        /* 0 未确认 / 1 已收 REGISTER_RESP（跨 3 线程） */
    _Atomic int     online;            /* 0 已报/未知 OFFLINE / 1 已报 ONLINE */
    _Atomic int64_t last_seen_ms;      /* CLOCK_MONOTONIC 毫秒（离线判定专用） */
    _Atomic int64_t next_reg_ms;       /* CLOCK_MONOTONIC 毫秒：下次注册重试最早时刻 */
    int             reg_fail_cnt;      /* 仅 upload_thread(tick) 访问 */
    _Atomic int     empty_skip_cnt;    /* publish_data（采样+续传两线程）→ 原子 */
};

static struct subdev_runtime g_subdev_rt[SUBDEVICE_MAX];
static int g_subdev_rt_count = 0;      /* = subdev_count()，加载期写定后只读 */

/* 未命中注册表节流计数（publish_data 两线程 → 原子） */
static _Atomic int g_unmatched_cnt = 0;

/* 线性查运行期槽（N≤16），未命中→NULL */
static struct subdev_runtime *rt_of(const struct subdev_entry *e)
{
    if (!e)
        return NULL;
    for (int i = 0; i < g_subdev_rt_count; i++)
        if (g_subdev_rt[i].e == e)
            return &g_subdev_rt[i];
    return NULL;
}

/* ─── Q2：装配入口（platform_select 内、线程创建前调用一次）────── */

int hw_subdev_init(const struct node_config *cfg)
{
    if (!cfg)
        return E_INVAL;

    memset(g_subdev_rt, 0, sizeof(g_subdev_rt));
    g_subdev_rt_count = 0;

    /* 注册表条目只有一份事实源（subdev_registry 的 g_entries） */
    int n = subdev_load(cfg->subdevices_conf, NULL, SUBDEVICE_MAX);
    if (n < 0)
        n = 0;
    if (n > SUBDEVICE_MAX)
        n = SUBDEVICE_MAX;

    for (int i = 0; i < n; i++)
        g_subdev_rt[i].e = subdev_at(i);
    g_subdev_rt_count = n;

    if (n == 0) {
        /* Q1-5：让 P1-16 缺口「可见」——醒目 WARN + 解析后的绝对路径 */
        LOG_WARN("huawei: subdevice registry '%s' has 0 entries -- "
                 "gateway-only mode, subdevice feature degraded",
                 cfg->subdevices_conf);
    } else {
        LOG_INFO("huawei: subdevice runtime ready: %d slot(s) from %s",
                 n, cfg->subdevices_conf);
    }
    return n;
}

int hw_subdev_rt_count(void)
{
    return g_subdev_rt_count;
}

int hw_subdev_stat_get(int idx, struct hw_subdev_stat *out)
{
    if (!out || idx < 0 || idx >= g_subdev_rt_count)
        return E_INVAL;

    struct subdev_runtime *rt = &g_subdev_rt[idx];
    memset(out, 0, sizeof(*out));
    if (rt->e)
        snprintf(out->device_id, sizeof(out->device_id), "%s", rt->e->device_id);
    out->registered     = atomic_load(&rt->registered);
    out->online         = atomic_load(&rt->online);
    out->reg_fail_cnt   = rt->reg_fail_cnt;
    out->empty_skip_cnt = atomic_load(&rt->empty_skip_cnt);
    out->last_seen_ms   = atomic_load(&rt->last_seen_ms);
    out->next_reg_ms    = atomic_load(&rt->next_reg_ms);
    return E_OK;
}

int64_t hw_subdev_backoff_ms(int fail_cnt)
{
    if (fail_cnt <= 0)
        fail_cnt = 1;
    int64_t v = (int64_t)fail_cnt * 30000;
    return (v > 300000) ? 300000 : v;   /* 30s*n 上限 300s */
}

/* ─── 内部发送助手 ──────────────────────────────────────────── */

static int send_register(const struct node_config *cfg, struct subdev_runtime *rt)
{
    char topic[256], payload[512];
    if (hw_build_topic(cfg, HW_TOPIC_REGISTER, topic, sizeof(topic)) != E_OK ||
        hw_build_subdev_register(rt->e, payload, sizeof(payload)) != E_OK) {
        LOG_WARN("huawei: build register for %s failed", rt->e->device_id);
        return E_IO;
    }
    int rc = pub_raw(topic, payload, 1);
    if (rc != E_OK)
        LOG_WARN("huawei: register publish failed for %s", rt->e->device_id);
    return rc;
}

static void report_status(const struct node_config *cfg,
                          struct subdev_runtime *rt, int online)
{
    char topic[256], payload[256];
    if (hw_build_topic(cfg, HW_TOPIC_STATUS, topic, sizeof(topic)) != E_OK ||
        hw_build_subdev_status(rt->e, online, payload, sizeof(payload)) != E_OK) {
        LOG_WARN("huawei: build status for %s failed", rt->e->device_id);
        return;
    }
    if (pub_raw(topic, payload, 1) == E_OK)
        atomic_store(&rt->online, online);
    else
        LOG_WARN("huawei: status %s publish failed for %s",
                 online ? "ONLINE" : "OFFLINE", rt->e->device_id);
}

/* ─── Q4-2：on_connected（网络线程）─────────────────────────── */

static int hw_on_connected(const struct node_config *cfg)
{
    if (!cfg)
        return E_INVAL;

    /* ① 订阅命令通配主题（mqtt_subscribe_topic 的首个调用者，闭环非阻塞-13） */
    char wc[256];
    if (hw_build_topic(cfg, HW_TOPIC_COMMANDS_WILDCARD, wc, sizeof(wc)) == E_OK) {
        if (mqtt_subscribe_topic(wc, 1) != E_OK)
            LOG_WARN("huawei: subscribe '%s' failed", wc);
    } else {
        LOG_WARN("huawei: build commands wildcard topic failed");
    }

    int64_t now = mono_ms();
    for (int i = 0; i < g_subdev_rt_count; i++) {
        struct subdev_runtime *rt = &g_subdev_rt[i];
        if (!rt->e)
            continue;

        if (atomic_load(&rt->registered) == 0) {
            /* ② 首次注册：发 register，置 next_reg_ms（重试计数由 tick 负责，
             *    避免 on_connected 跨线程写 reg_fail_cnt） */
            (void)send_register(cfg, rt);
            atomic_store(&rt->next_reg_ms, now);
        } else if (atomic_load(&rt->online) == 0) {
            /* ③ 已注册未报在线：补报 ONLINE */
            report_status(cfg, rt, 1);
        }
    }
    return E_OK;
}

/* ─── Q4-3：on_disconnected（网络线程；空实现）─────────────── */

static void hw_on_disconnected(void)
{
    /* 非阻塞-12：CONNACK 失败与真实断连两处都会触发，任何计数都会虚高。
     * 故不广播 OFFLINE、不做 churn 计数、不复位 registered（平台侧注册
     * 长期有效，§4.1）。churn 日志统一留 T05。 */
}

/* ─── Q4-4：on_message（网络线程）───────────────────────────── */

static void handle_register_resp(const struct node_config *cfg, const char *topic,
                                 const char *payload, int len)
{
    char rid[64];
    if (hw_extract_request_id(topic, rid, sizeof(rid)) != E_OK)
        rid[0] = '\0';

    /* payload 带长度、非 NUL 结尾：拷贝到本地 NUL 缓冲后按 device_id 匹配
     * （register 响应 schema 属联调校准项，docs/12 §9-2） */
    char buf[1024];
    int n = (len < (int)sizeof(buf) - 1) ? len : (int)sizeof(buf) - 1;
    if (n < 0)
        n = 0;
    memcpy(buf, payload, (size_t)n);
    buf[n] = '\0';

    int matched = 0;
    for (int i = 0; i < g_subdev_rt_count; i++) {
        struct subdev_runtime *rt = &g_subdev_rt[i];
        if (!rt->e || rt->e->device_id[0] == '\0')
            continue;
        if (strstr(buf, rt->e->device_id) != NULL) {
            atomic_store(&rt->registered, 1);
            matched++;
            if (atomic_load(&rt->online) == 0)
                report_status(cfg, rt, 1);
        }
    }

    if (matched == 0)
        LOG_WARN("huawei: register response matched no known subdevice "
                 "(rid=%s): %.80s", rid[0] ? rid : "?", buf);
    else
        LOG_INFO("huawei: register response ok (rid=%s), %d subdevice(s) confirmed",
                 rid[0] ? rid : "?", matched);
}

static void hw_on_message(const char *topic, const char *payload, int len)
{
    if (!topic || !payload)
        return;

    const struct node_config *cfg = platform_node_config();

    switch (hw_classify_topic(topic)) {
    case HW_KIND_REGISTER_RESP:
        if (cfg)
            handle_register_resp(cfg, topic, payload, len);
        else
            LOG_WARN("huawei: register response before platform_select (cfg NULL)");
        break;

    case HW_KIND_CMD_REQUEST: {
        char rid[64];
        int rc = hw_extract_request_id(topic, rid, sizeof(rid));
        LOG_INFO("huawei: command request received (rid=%s) -- deferred to T05",
                 rc == E_OK ? rid : "?");
        /* TODO(T05): 委派 platform_huawei_cmd：解析 paras → ota_handle_message/reboot
         *            → hw_build_command_response 回执。本轮仅日志，不执行命令。 */
        break;
    }

    default:
        /* 低频丢弃日志（如需更静默可加节流） */
        LOG_INFO("huawei: drop unhandled downstream topic '%s'", topic);
        break;
    }
}

/* ─── Q4-5：publish_status ─────────────────────────────────── */

static int hw_publish_status(const struct node_config *cfg,
                             const struct device_info *dev, const char *status)
{
    if (!cfg || !status)
        return E_INVAL;

    /* 裁决 b：网关属性数据源 = device_info；dev 缺省取注入的 device_info */
    const struct device_info *di = dev ? dev : platform_device_info();
    if (!di)
        return E_INVAL;

    char topic[256], payload[512];
    if (hw_build_topic(cfg, HW_TOPIC_PROPERTIES_REPORT, topic, sizeof(topic)) != E_OK)
        return E_IO;
    if (hw_build_gateway_props(cfg, di, status, payload, sizeof(payload)) != E_OK)
        return E_IO;
    return pub_raw(topic, payload, 1);
}

/* ─── Q4-6：publish_data（关键契约）────────────────────────── */

static int hw_publish_data(const struct node_config *cfg,
                           const struct sensor_data *data)
{
    if (!cfg || !data)
        return E_INVAL;

    /* 路由：本地传感器按 sensor_type；modbus 按 source_id(=slave_id) */
    const struct subdev_entry *e =
        (data->source == SOURCE_LOCAL) ? subdev_find_sensor(cfg->sensor_type)
                                       : subdev_find_modbus(data->source_id);

    if (!e) {
        /* 未命中注册表 → 永久不可路由 → 视为已处置（E_OK），不计入续传。
         * 节流 WARN，避免每采样点刷屏。 */
        int c = atomic_fetch_add(&g_unmatched_cnt, 1);
        if ((c % HW_THROTTLE_N) == 0)
            LOG_WARN("huawei: no subdevice for source=%s/%d, dropping "
                     "(throttled cnt=%d)",
                     data->source == SOURCE_LOCAL ? "sensor" : "modbus",
                     data->source == SOURCE_LOCAL ? 0 : data->source_id, c + 1);
        return E_OK;
    }

    char topic[256], payload[512];
    if (hw_build_topic(cfg, HW_TOPIC_BATCH, topic, sizeof(topic)) != E_OK) {
        LOG_WARN("huawei: build batch topic failed");
        return E_IO;
    }

    int rc = hw_build_batch_report(e, data, payload, sizeof(payload));
    if (rc == E_NOT_FOUND) {
        /* 方案 A 全空保护：跳过发布（不发空对象）+ 节流 WARN + 已处置 */
        struct subdev_runtime *rt = rt_of(e);
        if (rt) {
            int c = atomic_fetch_add(&rt->empty_skip_cnt, 1);
            if ((c % HW_THROTTLE_N) == 0)
                LOG_WARN("huawei: all fields invalid for %s, skip publish "
                         "(throttled cnt=%d)", e->device_id, c + 1);
        } else {
            LOG_WARN("huawei: all fields invalid for %s, skip publish",
                     e->device_id);
        }
        return E_OK;
    }
    if (rc != E_OK)
        return rc;   /* E_IO 截断 → 瞬态（非 E_OK） */

    if (pub_raw(topic, payload, 1) != E_OK)
        return E_NET;   /* 发布失败 → 瞬态（非 E_OK），续传可重试 */

    /* 成功：刷新 last_seen（单调钟），必要时补报 ONLINE */
    struct subdev_runtime *rt = rt_of(e);
    if (rt) {
        atomic_store(&rt->last_seen_ms, mono_ms());
        if (atomic_load(&rt->online) == 0)
            report_status(cfg, rt, 1);
    }
    return E_OK;
}

/* ─── Q4-7：publish_alert ──────────────────────────────────── */

static int hw_publish_alert(const struct node_config *cfg,
                            const struct alert_event *evt)
{
    if (!cfg || !evt)
        return E_INVAL;

    /* source_id 字符串：本地传感器用 sensor_type；modbus 用 slave_id 十进制 */
    char source_id[64];
    if (strcmp(evt->source_kind, "modbus") == 0)
        snprintf(source_id, sizeof(source_id), "%d", evt->source_id);
    else
        snprintf(source_id, sizeof(source_id), "%s", cfg->sensor_type);

    char topic[256], payload[768];
    if (hw_build_topic(cfg, HW_TOPIC_EVENTS_REPORT, topic, sizeof(topic)) != E_OK) {
        LOG_WARN("huawei: build events topic failed");
        return E_IO;
    }
    int rc = hw_build_event_alert(evt, source_id, payload, sizeof(payload));
    if (rc != E_OK) {
        LOG_WARN("huawei: build event alert failed (%d)", rc);
        return rc;
    }
    return pub_raw(topic, payload, 1);
}

/* ─── Q4-8：tick（upload_thread，5s）───────────────────────── */

static void hw_tick(const struct node_config *cfg)
{
    if (!cfg)
        return;

    int64_t now = mono_ms();

    /* ① 网关属性周期上报（内部节流 huawei_props_interval，默认 60s） */
    static int64_t s_last_props_ms = 0;
    int interval = (cfg->huawei_props_interval > 0) ? cfg->huawei_props_interval : 60;
    if (s_last_props_ms == 0 || now - s_last_props_ms >= (int64_t)interval * 1000) {
        const struct device_info *dev = platform_device_info();
        if (dev && hw_publish_status(cfg, dev, "online") == E_OK)
            s_last_props_ms = now;
    }

    /* ② 离线扫描：last_seen 非 0 且超时且当前在线 → OFFLINE */
    int64_t offline_ms = (int64_t)cfg->subdev_offline_sec * 1000;
    for (int i = 0; i < g_subdev_rt_count; i++) {
        struct subdev_runtime *rt = &g_subdev_rt[i];
        if (!rt->e)
            continue;
        int64_t ls = atomic_load(&rt->last_seen_ms);
        if (ls != 0 && atomic_load(&rt->online) == 1 && now - ls > offline_ms)
            report_status(cfg, rt, 0);
    }

    /* ③ 注册退避重试（仅 registered==0；reg_fail_cnt 仅本线程） */
    for (int i = 0; i < g_subdev_rt_count; i++) {
        struct subdev_runtime *rt = &g_subdev_rt[i];
        if (!rt->e)
            continue;
        if (atomic_load(&rt->registered) != 0)
            continue;
        if (now < atomic_load(&rt->next_reg_ms))
            continue;

        if (send_register(cfg, rt) != E_OK) {
            rt->reg_fail_cnt++;
            int64_t off = hw_subdev_backoff_ms(rt->reg_fail_cnt);
            atomic_store(&rt->next_reg_ms, now + off);
            LOG_WARN("huawei: register retry for %s failed "
                     "(fail=%d, next in %lldms)",
                     rt->e->device_id, rt->reg_fail_cnt, (long long)off);
        } else {
            /* 已重发，等待 REGISTER_RESP；下轮退避 30s 后再评估 */
            atomic_store(&rt->next_reg_ms, now + hw_subdev_backoff_ms(1));
            LOG_INFO("huawei: re-sent register for %s", rt->e->device_id);
        }
    }
}

/* ─── Q4-9：shutdown ───────────────────────────────────────── */

static void hw_shutdown(void)
{
    /* 不清 socket（mqtt_close 由 main 统一；同 platform_local 语义） */
    memset(g_subdev_rt, 0, sizeof(g_subdev_rt));
    g_subdev_rt_count = 0;
}

/* ─── ops 表（Q4；与 platform_local.c 同构 9 成员）──────────── */

const struct platform_ops platform_huawei_ops = {
    .name            = "huawei",
    .connect_params  = hw_connect_params,
    .on_connected    = hw_on_connected,
    .on_disconnected = hw_on_disconnected,
    .on_message      = hw_on_message,
    .publish_status  = hw_publish_status,
    .publish_data    = hw_publish_data,
    .publish_alert   = hw_publish_alert,
    .tick            = hw_tick,
    .shutdown        = hw_shutdown,
};
