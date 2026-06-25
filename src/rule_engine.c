/*
 * rule_engine.c
 * 规则引擎核心实现（阶段三）
 *
 * 功能：
 *   - 从 node_config 加载规则（gt / lt / eq / ne / between / rate）
 *   - 对传感器数据逐条评估
 *   - 冷却时间（cooldown）防重复告警
 *   - 变化率（rate）使用滑动窗口 + 环形缓冲区
 *   - 返回动作掩码，由调用方执行 MQTT 告警 / GPIO 控制
 *   - 统计数据收集（供 Web Dashboard 查询）
 */

#include "rule_engine.h"
#include <math.h>

/* ─── 内部状态 ─────────────────────────────────────────── */

static struct rule  g_rules[RULE_MAX];       /* 规则副本（含运行时状态） */
static int          g_rule_count = 0;         /* 已加载规则数 */
static int          g_initialized = 0;        /* 初始化标志 */
static struct rule_stats g_stats[RULE_MAX];   /* 统计信息 */

/* ─── 时间戳获取（单调时钟，不受系统时间调整影响）─────── */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ─── 从 sensor_data 读取字段值 ────────────────────────── */

static double get_field(const struct sensor_data *data, const char *field)
{
    if (strcmp(field, "temperature") == 0)
        return data->temperature;
    if (strcmp(field, "humidity") == 0)
        return data->humidity;
    if (strcmp(field, "pressure") == 0)
        return data->pressure;
    return -999.0;   /* 未知字段，永不为真 */
}

/* ─── 变化率环形缓冲区操作 ─────────────────────────────── */

static void rate_history_push(struct rule *r, double value, int64_t ts)
{
    r->rate_history[r->rate_head]    = value;
    r->rate_timestamps[r->rate_head] = ts;
    r->rate_head = (r->rate_head + 1) % 16;
    if (r->rate_count < 16)
        r->rate_count++;
}

/*
 * 计算当前值相对于滑动窗口内最老值的绝对变化量。
 * 返回 fabs(current_val - oldest_val_in_window)。
 */
static double rate_calculate(const struct rule *r,
                              double current_val, int64_t now)
{
    double  window_s = r->rate_window_s;
    int64_t cutoff   = now - (int64_t)(window_s * 1000.0);

    double oldest_val = current_val;
    int    found      = 0;

    /* 遍历环形缓冲区，找到窗口内最老的一条记录 */
    for (int i = 0; i < r->rate_count; i++) {
        int idx = (r->rate_head - 1 - i + 16) % 16;
        if (r->rate_timestamps[idx] >= cutoff) {
            oldest_val = r->rate_history[idx];
            found      = 1;
        }
        /* 时间戳在窗口外的直接停止（越往前越老）*/
        /* 注意：环形缓冲区按插入顺序排列，head-1 是最新的 */
    }

    /* 没有任何数据在窗口内也没关系，返回 0（变化量 = 0） */
    if (!found)
        return 0.0;

    return fabs(current_val - oldest_val);
}

/* ─── 单规则匹配 ────────────────────────────────────────── */

static int rule_match(const struct rule *r,
                      const struct sensor_data *data, int64_t now)
{
    double val = get_field(data, r->field);
    if (val == -999.0)
        return 0;   /* 未知字段，永不匹配 */

    switch (r->op) {

    case OP_GT:
        return val > r->threshold;

    case OP_LT:
        return val < r->threshold;

    case OP_EQ:
        return fabs(val - r->threshold) < 0.001;

    case OP_NE:
        return fabs(val - r->threshold) >= 0.001;

    case OP_BETWEEN:
        /* 值在 [lo, hi] 区间外 即触发 */
        return (val < r->threshold_lo || val > r->threshold_hi);

    case OP_RATE: {
        /* 先存入历史值，再计算变化率 */
        rate_history_push((struct rule *)r, val, now);
        double rate = rate_calculate(r, val, now);
        return rate > r->threshold;
    }

    default:
        return 0;
    }
}

/* ─── 告警消息生成 ─────────────────────────────────────── */

static void gen_alert_msg(const struct rule *r,
                          const struct sensor_data *data,
                          char *msg, int msg_len)
{
    double val = get_field(data, r->field);
    const char *op_str = "?";

    switch (r->op) {
    case OP_GT:      op_str = ">";       break;
    case OP_LT:      op_str = "<";       break;
    case OP_EQ:      op_str = "==";      break;
    case OP_NE:      op_str = "!=";      break;
    case OP_BETWEEN: op_str = "outside"; break;
    case OP_RATE:    op_str = "rate>";   break;
    }

    if (r->op == OP_BETWEEN) {
        snprintf(msg, msg_len,
                 "ALERT [%s] %s=%.2f %s [%.2f,%.2f]",
                 r->name, r->field, val,
                 op_str, r->threshold_lo, r->threshold_hi);
    } else if (r->op == OP_RATE) {
        snprintf(msg, msg_len,
                 "ALERT [%s] %s change-rate > %.2f / %.0fs",
                 r->name, r->field,
                 r->threshold, r->rate_window_s);
    } else {
        snprintf(msg, msg_len,
                 "ALERT [%s] %s=%.2f %s %.2f",
                 r->name, r->field, val, op_str, r->threshold);
    }
}

/* ═══════════════════════════════════════════════════════════
 * 公开 API
 * ═══════════════════════════════════════════════════════════ */

int rule_engine_init(const struct node_config *cfg)
{
    if (!cfg)
        return E_INVAL;
    if (cfg->rule_count < 0 || cfg->rule_count > RULE_MAX)
        return E_INVAL;

    g_rule_count = cfg->rule_count;

    for (int i = 0; i < g_rule_count; i++) {
        /* 深拷贝规则定义（保留配置中的静态字段） */
        memcpy(&g_rules[i], &cfg->rules[i], sizeof(struct rule));

        /* 重置运行时状态 */
        g_rules[i].last_triggered = 0;
        g_rules[i].rate_head      = 0;
        g_rules[i].rate_count     = 0;
        memset(g_rules[i].rate_history, 0, sizeof(g_rules[i].rate_history));
        memset(g_rules[i].rate_timestamps, 0, sizeof(g_rules[i].rate_timestamps));

        /* 初始化统计 */
        memset(&g_stats[i], 0, sizeof(g_stats[i]));
        memcpy(g_stats[i].name, g_rules[i].name, sizeof(g_stats[i].name));
        g_stats[i].name[sizeof(g_stats[i].name) - 1] = '\0';
    }

    g_initialized = 1;

    LOG_INFO("rule engine init ok: %d rules loaded", g_rule_count);
    for (int i = 0; i < g_rule_count; i++) {
        const char *op_name = "?";
        switch (g_rules[i].op) {
        case OP_GT:      op_name = "gt";      break;
        case OP_LT:      op_name = "lt";      break;
        case OP_EQ:      op_name = "eq";      break;
        case OP_NE:      op_name = "ne";      break;
        case OP_BETWEEN: op_name = "between"; break;
        case OP_RATE:    op_name = "rate";    break;
        }
        LOG_INFO("  rule[%d] '%s': %s %s th=%.2f act=0x%02x cd=%dms",
                 i, g_rules[i].name, g_rules[i].field, op_name,
                 g_rules[i].threshold,
                 g_rules[i].action_mask, g_rules[i].cooldown_ms);
    }

    return E_OK;
}

uint8_t rule_engine_evaluate(const struct sensor_data *data,
                              char *alert_msg, int alert_msg_len)
{
    if (!g_initialized || !data)
        return 0;

    int64_t now          = now_ms();
    uint8_t triggered    = 0;
    int     alert_written = 0;

    for (int i = 0; i < g_rule_count; i++) {
        struct rule *r = &g_rules[i];

        /* ── 冷却时间检查 ── */
        if (r->cooldown_ms > 0 && r->last_triggered > 0) {
            if ((now - r->last_triggered) < (int64_t)r->cooldown_ms)
                continue;   /* 冷却中，跳过 */
        }

        /* ── 规则匹配 ── */
        if (rule_match(r, data, now)) {
            r->last_triggered = now;
            g_stats[i].trigger_count++;
            g_stats[i].last_triggered = now;
            triggered |= r->action_mask;

            LOG_INFO("rule triggered: '%s' (count=%d, act=0x%02x)",
                     r->name, g_stats[i].trigger_count,
                     r->action_mask);

            /* 仅第一条触发规则生成告警消息 */
            if (!alert_written && alert_msg && alert_msg_len > 0) {
                gen_alert_msg(r, data, alert_msg, alert_msg_len);
                alert_written = 1;
            }
        }
    }

    return triggered;
}

int rule_engine_get_stats(struct rule_stats *stats, int max_count)
{
    if (!stats || max_count <= 0)
        return E_INVAL;

    int n = (g_rule_count < max_count) ? g_rule_count : max_count;
    for (int i = 0; i < n; i++) {
        memcpy(&stats[i], &g_stats[i], sizeof(struct rule_stats));
    }
    return n;
}

void rule_engine_close(void)
{
    g_initialized = 0;
    g_rule_count  = 0;
    memset(g_stats, 0, sizeof(g_stats));
    memset(g_rules, 0, sizeof(g_rules));
    LOG_INFO("rule engine closed");
}
