/*
 * anomaly_engine.c
 * 异常检测引擎核心实现（方向 B 阶段一：Z-score 统计层）
 *
 * 功能：
 *   - 从 node_config 加载异常检测规则
 *   - 滑动窗口统计（均值 + 标准差）
 *   - Z-score 计算与阈值比较
 *   - 冷却时间（cooldown）防重复告警
 *   - 返回动作掩码，由调用方执行 MQTT 告警 / GPIO 控制
 *   - 统计数据收集（供 Web Dashboard 查询）
 *
 * 后续叠加（阶段二）：Isolation Forest 推理引擎
 */

#include "anomaly_engine.h"
#include "iforest_model.h"
#include <math.h>

/* ─── 内部状态 ─────────────────────────────────────────── */

static struct anomaly_config g_anoms[ANOMALY_MAX];
static int                   g_anomaly_count = 0;
static int                   g_initialized   = 0;
static struct anomaly_stats  g_stats[ANOMALY_MAX];

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

/* ─── 滑动窗口操作 ─────────────────────────────────────── */

/*
 * 向环形缓冲区推入一个新值
 */
static void window_push(struct anomaly_config *a, double val)
{
    a->window[a->window_head] = val;
    a->window_head = (a->window_head + 1) % a->window_size;
    if (a->window_count < a->window_size)
        a->window_count++;
}

/*
 * 从滑动窗口中计算均值与标准差
 * 返回: mean 和 std 通过指针输出
 * std == 0 表示窗口不足 2 个样本或数据无变化
 */
static void window_stats(const struct anomaly_config *a,
                         double *mean, double *std)
{
    int n = a->window_count;
    *mean = 0.0;
    *std  = 0.0;

    if (n < 2)
        return;

    /* 计算均值 */
    double sum = 0.0;
    for (int i = 0; i < n; i++)
        sum += a->window[i];
    *mean = sum / (double)n;

    /* 计算标准差（总体标准差） */
    double sum_sq = 0.0;
    for (int i = 0; i < n; i++) {
        double diff = a->window[i] - *mean;
        sum_sq += diff * diff;
    }
    *std = sqrt(sum_sq / (double)n);
}

/* ─── Isolation Forest 推理引擎 ─────────────────────────── */

/* 调和数近似: H(i) = ln(i) + Euler-Mascheroni constant */
static double harmonic(int n)
{
    if (n <= 1) return 0.0;
    return log((double)n) + 0.5772156649;
}

/* 平均路径长度归一化因子 c(n) */
static double c_factor(int n)
{
    if (n <= 1) return 0.0;
    double h = harmonic(n - 1);
    return 2.0 * h - 2.0 * (double)(n - 1) / (double)n;
}

/*
 * 遍历一棵 iForest 树，返回样本在该树中的路径长度（深度）
 * tree_base: 这棵树在 iforest_trees[] 中的起始索引
 * features:  [temperature, humidity, pressure]
 */
static int tree_path_length(int tree_base, const double *features)
{
    int depth = 0;
    int node  = 0;

    while (1) {
        int base = tree_base + 1 + node * 4;  /* 跳过 node_count */
        int feat = (int)iforest_trees[base];
        float split = iforest_trees[base + 1];
        int left    = (int)iforest_trees[base + 2];
        int right   = (int)iforest_trees[base + 3];

        if (feat == -1) break;  /* 叶节点 */
        depth++;

        if (features[feat] <= split)
            node = left;
        else
            node = right;
    }
    return depth;
}

/*
 * iForest 异常分数预测
 * 返回: 0.0 ~ 1.0，值越大越异常。
 *       返回 -1.0 表示模型未加载（IFOREST_AVAILABLE == 0）。
 */
static double iforest_predict(const struct sensor_data *data)
{
    if (!IFOREST_AVAILABLE) {
        static int warned = 0;
        if (!warned) {
            LOG_WARN("iforest model not available "
                     "(run tools/anomaly_train.py)");
            warned = 1;
        }
        return -1.0;
    }

    double features[3];
    features[0] = data->temperature;
    features[1] = data->humidity;
    features[2] = data->pressure;

    double total_path = 0.0;
    for (int t = 0; t < IFOREST_N_TREES; t++) {
        int tree_base = iforest_tree_offsets[t];
        int path = tree_path_length(tree_base, features);
        total_path += (double)path;
    }

    double avg_path = total_path / (double)IFOREST_N_TREES;
    double c = c_factor(IFOREST_MAX_NODES > 0 ? IFOREST_MAX_NODES : 256);

    /* 异常分数: s(x,n) = 2^(-E(h(x))/c(n))
     * 分数 → 0: 正常（路径短）
     * 分数 → 1: 异常（路径长，被快速隔离）*/
    double score = pow(2.0, -avg_path / c);
    return score;
}

/* ─── 单规则异常检测 ──────────────────────────────────── */

/*
 * 对一条异常规则评估当前数据
 * 返回: 1=异常触发, 0=正常
 * zscore_out: 可选，输出计算出的异常分数
 */
static int anomaly_match(const struct anomaly_config *a,
                          const struct sensor_data *data,
                          double *zscore_out)
{
    double val = get_field(data, a->field);
    if (val == -999.0)
        return 0;   /* 未知字段 */

    /* ── iForest 分支 ── */
    if (a->algo == ANOMALY_IFOREST) {
        double score = iforest_predict(data);
        if (zscore_out) *zscore_out = score;
        if (score < 0.0) return 0;  /* 模型不可用 */
        return (score > a->zscore_threshold) ? 1 : 0;
    }

    /* ── Z-score 分支 ── */

    /* 保存 push 前的窗口计数：stats 基于旧窗口计算，检验也应用旧计数 */
    int old_count = a->window_count;

    /* 先在现有窗口上计算基线（不含当前值，避免异常值污染基线） */
    double mean, std;
    window_stats(a, &mean, &std);

    /* 推入新值（在判断之后，保证窗口持续更新） */
    window_push((struct anomaly_config *)a, val);

    /* 窗口不足，无法判断（用旧计数，因为 stats 基于旧窗口） */
    if (old_count < 2) {
        if (zscore_out) *zscore_out = 0.0;
        return 0;
    }

    /* 若标准差为零（所有数据完全相同）：
     *   当前值有任何偏离即视为异常（偏离 > 0.001 即 flag） */
    double z;
    if (std < 0.0001) {
        double diff = fabs(val - mean);
        z = (diff > 0.001) ? (diff / 0.001) : 0.0;
    } else {
        z = fabs(val - mean) / std;
    }

    /* 更新基线（供外部查询） */
    ((struct anomaly_config *)a)->baseline_mean = mean;
    ((struct anomaly_config *)a)->baseline_std  = std;

    if (zscore_out)
        *zscore_out = z;

    return (z > a->zscore_threshold) ? 1 : 0;
}

/* ─── 告警消息生成 ─────────────────────────────────────── */

static void gen_alert_msg(const struct anomaly_config *a,
                           const struct sensor_data *data,
                           double score,
                           char *msg, int msg_len)
{
    double val = get_field(data, a->field);

    if (a->algo == ANOMALY_IFOREST) {
        snprintf(msg, msg_len,
                 "ANOMALY [%s] %s=%.2f iforest=%.4f > %.2f",
                 a->name, a->field, val, score,
                 a->zscore_threshold);
    } else {
        snprintf(msg, msg_len,
                 "ANOMALY [%s] %s=%.2f zscore=%.4f > %.2f "
                 "(mean=%.2f std=%.2f n=%d)",
                 a->name, a->field, val, score,
                 a->zscore_threshold,
                 a->baseline_mean, a->baseline_std,
                 a->window_count);
    }
}

/* ═══════════════════════════════════════════════════════════
 * 公开 API
 * ═══════════════════════════════════════════════════════════ */

int anomaly_engine_init(const struct node_config *cfg)
{
    if (!cfg)
        return E_INVAL;
    if (cfg->anomaly_count < 0 || cfg->anomaly_count > ANOMALY_MAX)
        return E_INVAL;

    g_anomaly_count = cfg->anomaly_count;

    if (!cfg->anomaly_enabled || g_anomaly_count == 0) {
        g_initialized = 1;
        LOG_INFO("anomaly engine: disabled or no rules, init skipped");
        return E_OK;
    }

    for (int i = 0; i < g_anomaly_count; i++) {
        /* 深拷贝规则定义（保留配置中的静态字段） */
        memcpy(&g_anoms[i], &cfg->anoms[i], sizeof(struct anomaly_config));

        /* 重置运行时状态 */
        g_anoms[i].window_head   = 0;
        g_anoms[i].window_count  = 0;
        g_anoms[i].baseline_mean = 0.0;
        g_anoms[i].baseline_std  = 0.0;
        g_anoms[i].last_triggered = 0;
        g_anoms[i].trigger_count  = 0;
        memset(g_anoms[i].window, 0, sizeof(g_anoms[i].window));

        /* 确保 window_size 在合法范围 */
        if (g_anoms[i].window_size <= 0 ||
            g_anoms[i].window_size > ANOMALY_WINDOW_SIZE)
            g_anoms[i].window_size = ANOMALY_WINDOW_SIZE;

        /* 初始化统计 */
        memset(&g_stats[i], 0, sizeof(g_stats[i]));
        memcpy(g_stats[i].name, g_anoms[i].name, sizeof(g_stats[i].name));
        g_stats[i].name[sizeof(g_stats[i].name) - 1] = '\0';
    }

    g_initialized = 1;

    LOG_INFO("anomaly engine init ok: %d rules loaded", g_anomaly_count);
    for (int i = 0; i < g_anomaly_count; i++) {
        const char *algo_name =
            (g_anoms[i].algo == ANOMALY_ZSCORE)  ? "zscore" :
            (g_anoms[i].algo == ANOMALY_IFOREST) ? "iforest" : "?";
        LOG_INFO("  anomaly[%d] '%s': %s %s th=%.2f act=0x%02x "
                 "cd=%dms win=%d",
                 i, g_anoms[i].name, g_anoms[i].field, algo_name,
                 g_anoms[i].zscore_threshold,
                 g_anoms[i].action_mask, g_anoms[i].cooldown_ms,
                 g_anoms[i].window_size);
    }

    return E_OK;
}

uint8_t anomaly_engine_evaluate(const struct sensor_data *data,
                                 char *alert_msg, int alert_msg_len)
{
    if (!g_initialized || !data)
        return 0;

    int64_t now          = now_ms();
    uint8_t triggered    = 0;
    int     alert_written = 0;

    for (int i = 0; i < g_anomaly_count; i++) {
        struct anomaly_config *a = &g_anoms[i];

        /* ── 冷却时间检查 ── */
        if (a->cooldown_ms > 0 && a->last_triggered > 0) {
            if ((now - a->last_triggered) < (int64_t)a->cooldown_ms)
                continue;   /* 冷却中，跳过 */
        }

        /* ── 异常检测 ── */
        double score = 0.0;
        int    hit   = anomaly_match(a, data, &score);

        /* 更新实时统计（无论是否触发） */
        g_stats[i].current_zscore = score;
        g_stats[i].current_score  = score;

        if (hit) {
            a->last_triggered = now;
            a->trigger_count++;
            g_stats[i].trigger_count++;
            g_stats[i].last_triggered = now;
            triggered |= a->action_mask;

            LOG_INFO("anomaly triggered: '%s' (count=%d, act=0x%02x, "
                     "score=%.4f)",
                     a->name, g_stats[i].trigger_count,
                     a->action_mask, score);

            /* 仅第一条触发规则生成告警消息 */
            if (!alert_written && alert_msg && alert_msg_len > 0) {
                gen_alert_msg(a, data, score, alert_msg, alert_msg_len);
                alert_written = 1;
            }
        }
    }

    return triggered;
}

int anomaly_engine_get_stats(struct anomaly_stats *stats, int max_count)
{
    if (!stats || max_count <= 0)
        return E_INVAL;

    int n = (g_anomaly_count < max_count) ? g_anomaly_count : max_count;
    for (int i = 0; i < n; i++) {
        memcpy(&stats[i], &g_stats[i], sizeof(struct anomaly_stats));
    }
    return n;
}

void anomaly_engine_close(void)
{
    g_initialized   = 0;
    g_anomaly_count = 0;
    memset(g_stats,  0, sizeof(g_stats));
    memset(g_anoms,  0, sizeof(g_anoms));
    LOG_INFO("anomaly engine closed");
}
