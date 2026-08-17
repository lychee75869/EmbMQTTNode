/*
 * tests/test_anomaly_engine.c
 * 异常检测引擎单元测试（方向 B）
 *
 * 覆盖:
 *   - Z-score 基本检测（基线建立 → 异常注入）
 *   - 滑动窗口统计更新
 *   - 冷却时间防抖
 *   - 动作掩码组合
 *   - 边界条件（NULL/empty/closed/std=0）
 *   - 配置解析（anomaly_1/2/3 via config_load）
 *   - 统计数据收集
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <math.h>
#include "../src/common.h"
#include "../src/anomaly_engine.h"
#include "../src/config.h"
#include "../src/iforest_model.h"

/* 辅助: 构造一条 sensor_data */
static struct sensor_data make_data(double temp, double hum, double pres)
{
    struct sensor_data d;
    memset(&d, 0, sizeof(d));
    d.temperature   = temp;
    d.humidity      = hum;
    d.pressure      = pres;
    d.timestamp_ms  = 1000000;
    return d;
}

/* 辅助: 构造一条 anomaly_config */
static void make_anomaly(struct anomaly_config *a, const char *name,
                          const char *field, enum anomaly_algo algo,
                          double zscore_th, uint8_t action, int cooldown_ms)
{
    memset(a, 0, sizeof(*a));
    strncpy(a->name,  name,  ANOMALY_NAME_LEN - 1);
    strncpy(a->field, field, ANOMALY_FIELD_LEN - 1);
    a->algo             = algo;
    a->zscore_threshold = zscore_th;
    a->action_mask      = action;
    a->cooldown_ms      = cooldown_ms;
    a->window_size      = ANOMALY_WINDOW_SIZE;
}

/* ═══════════════════════════════════════════════════════════
 * 测试 1: Z-score 基本检测
 * 策略：先推入 20 个等值样本建立基线（25.0℃），再用一个
 *       远超 3σ 的值（50.0℃）触发检测
 * ═══════════════════════════════════════════════════════════ */
static void test_zscore_basic(void)
{
    printf("--- test_zscore_basic ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.anomaly_enabled = 1;
    cfg.anomaly_count   = 1;

    make_anomaly(&cfg.anoms[0], "a_temp", "temperature",
                 ANOMALY_ZSCORE, 2.5, ACTION_LOG_ONLY, 0);

    assert(anomaly_engine_init(&cfg) == E_OK);

    /* Phase 1: 建立基线 — 20 个正常值 */
    for (int i = 0; i < 20; i++) {
        struct sensor_data d = make_data(25.0, 55.0, 1013.0);
        uint8_t act = anomaly_engine_evaluate(&d, NULL, 0);
        assert(act == 0);  /* 基线稳定，不触发 */
    }
    printf("  baseline built (20 samples of 25.0C): PASS\n");

    /* Phase 2: 注入异常 — 温度突跳到 50℃ */
    struct sensor_data anomaly = make_data(50.0, 55.0, 1013.0);
    uint8_t act = anomaly_engine_evaluate(&anomaly, NULL, 0);
    assert(act & ACTION_LOG_ONLY);
    printf("  anomaly (50.0C) triggered:            PASS\n");

    /* Phase 3: 返回正常值 — 不触发 */
    struct sensor_data normal = make_data(25.0, 55.0, 1013.0);
    act = anomaly_engine_evaluate(&normal, NULL, 0);
    assert(act == 0);
    printf("  normal (25.0C) not triggered:         PASS\n");

    anomaly_engine_close();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 2: 滑动窗口统计验证
 * 窗口 size=10，推入 [20,22,24,26,28,30,32,34,36,38]
 * 均值=29, 标准差≈5.74
 * 插入 50.0 → z≈(50-29)/5.74≈3.66 → 超过阈值 3.0 触发
 * ═══════════════════════════════════════════════════════════ */
static void test_window_sliding(void)
{
    printf("--- test_window_sliding ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.anomaly_enabled = 1;
    cfg.anomaly_count   = 1;

    make_anomaly(&cfg.anoms[0], "a_win", "temperature",
                 ANOMALY_ZSCORE, 3.0, ACTION_LOG_ONLY, 0);
    cfg.anoms[0].window_size = 10;  /* 小窗口，加速测试 */

    assert(anomaly_engine_init(&cfg) == E_OK);

    /* 填充窗口: 20,22,24,26,28,30,32,34,36,38 */
    for (int i = 0; i < 10; i++) {
        struct sensor_data d = make_data(20.0 + i * 2.0, 55.0, 1013.0);
        anomaly_engine_evaluate(&d, NULL, 0);
    }

    /* 检查统计量 */
    struct anomaly_stats stats[1];
    assert(anomaly_engine_get_stats(stats, 1) == 1);
    printf("  window stats: z=%.4f (mean≈29, std≈6.06)\n",
           stats[0].current_zscore);

    /* 插入一个偏离值: 50.0 → z ≈ (50-29)/6.06 ≈ 3.46 > 3.0 */
    struct sensor_data outlier = make_data(50.0, 55.0, 1013.0);
    uint8_t act = anomaly_engine_evaluate(&outlier, NULL, 0);
    assert(act & ACTION_LOG_ONLY);
    printf("  outlier (50.0) triggered:   PASS\n");

    /* 验证窗口计数 */
    anomaly_engine_get_stats(stats, 1);
    printf("  zscore after outlier: %.4f\n", stats[0].current_zscore);

    anomaly_engine_close();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 3: 冷却时间
 * ═══════════════════════════════════════════════════════════ */
static void test_cooldown(void)
{
    printf("--- test_cooldown ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.anomaly_enabled = 1;
    cfg.anomaly_count   = 1;

    /* 冷却 500ms */
    make_anomaly(&cfg.anoms[0], "a_cd", "temperature",
                 ANOMALY_ZSCORE, 1.5, ACTION_LOG_ONLY, 500);

    assert(anomaly_engine_init(&cfg) == E_OK);

    /* 建立基线 */
    for (int i = 0; i < 10; i++) {
        struct sensor_data d = make_data(25.0, 55.0, 1013.0);
        anomaly_engine_evaluate(&d, NULL, 0);
    }

    /* 第一次触发 */
    struct sensor_data d1 = make_data(50.0, 55.0, 1013.0);
    uint8_t act = anomaly_engine_evaluate(&d1, NULL, 0);
    assert(act & ACTION_LOG_ONLY);
    printf("  first trigger:               PASS\n");

    /* 立即再评估，应在冷却期内 */
    act = anomaly_engine_evaluate(&d1, NULL, 0);
    assert(act == 0);
    printf("  cooldown suppressed:         PASS\n");

    /* 等待冷却结束 */
    {
        struct timespec ts = {0, 600000000};  /* 600ms > 500ms */
        nanosleep(&ts, NULL);
    }

    /* 冷却结束后再次触发 */
    act = anomaly_engine_evaluate(&d1, NULL, 0);
    assert(act & ACTION_LOG_ONLY);
    printf("  after cooldown triggered:    PASS\n");

    anomaly_engine_close();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 4: 边界条件
 * ═══════════════════════════════════════════════════════════ */
static void test_edge_cases(void)
{
    printf("--- test_edge_cases ---\n");

    /* NULL 参数 */
    assert(anomaly_engine_init(NULL) == E_INVAL);
    printf("  NULL init:                     PASS\n");

    /* 禁用状态 + 空规则集 */
    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.anomaly_enabled = 0;
        cfg.anomaly_count   = 0;
        assert(anomaly_engine_init(&cfg) == E_OK);
        struct sensor_data d = make_data(25.0, 50.0, 1013.0);
        assert(anomaly_engine_evaluate(&d, NULL, 0) == 0);
        printf("  disabled + empty rules:       PASS\n");
        anomaly_engine_close();
    }

    /* 未初始化时调用 evaluate */
    assert(anomaly_engine_evaluate(
               &(struct sensor_data){25.0, 50.0, 1013.0, 0},
               NULL, 0) == 0);
    printf("  evaluate while closed:         PASS\n");

    /* NULL data */
    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.anomaly_enabled = 1;
        cfg.anomaly_count   = 1;
        make_anomaly(&cfg.anoms[0], "r", "temperature",
                     ANOMALY_ZSCORE, 3.0, ACTION_LOG_ONLY, 0);
        assert(anomaly_engine_init(&cfg) == E_OK);
        assert(anomaly_engine_evaluate(NULL, NULL, 0) == 0);
        printf("  NULL data:                    PASS\n");
        anomaly_engine_close();
    }

    /* 窗口不足（1 个样本）：不触发 */
    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.anomaly_enabled = 1;
        cfg.anomaly_count   = 1;
        make_anomaly(&cfg.anoms[0], "r", "temperature",
                     ANOMALY_ZSCORE, 1.0, ACTION_LOG_ONLY, 0);
        assert(anomaly_engine_init(&cfg) == E_OK);
        struct sensor_data d = make_data(100.0, 50.0, 1013.0);
        uint8_t act = anomaly_engine_evaluate(&d, NULL, 0);
        assert(act == 0);  /* 仅 1 个样本，窗口不足 */
        printf("  insufficient window (n=1):     PASS\n");
        anomaly_engine_close();
    }

    /* 未知字段 */
    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.anomaly_enabled = 1;
        cfg.anomaly_count   = 1;
        make_anomaly(&cfg.anoms[0], "r", "nonexistent",
                     ANOMALY_ZSCORE, 1.0, ACTION_LOG_ONLY, 0);
        assert(anomaly_engine_init(&cfg) == E_OK);
        struct sensor_data d = make_data(25.0, 50.0, 1013.0);
        uint8_t act = anomaly_engine_evaluate(&d, NULL, 0);
        assert(act == 0);  /* 未知字段永不触发 */
        printf("  unknown field:                PASS\n");
        anomaly_engine_close();
    }
}

/* ═══════════════════════════════════════════════════════════
 * 测试 5: 动作掩码
 * ═══════════════════════════════════════════════════════════ */
static void test_action_masks(void)
{
    printf("--- test_action_masks ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.anomaly_enabled = 1;
    cfg.anomaly_count   = 2;

    make_anomaly(&cfg.anoms[0], "a_mqtt", "temperature",
                 ANOMALY_ZSCORE, 2.0, ACTION_ALERT_MQTT, 0);
    make_anomaly(&cfg.anoms[1], "a_gpio", "humidity",
                 ANOMALY_ZSCORE, 2.0, ACTION_GPIO_1 | ACTION_GPIO_2, 0);

    assert(anomaly_engine_init(&cfg) == E_OK);

    /* 建立基线 */
    for (int i = 0; i < 10; i++) {
        struct sensor_data d = make_data(25.0, 55.0, 1013.0);
        anomaly_engine_evaluate(&d, NULL, 0);
    }

    /* 只有温度异常 → 只触发 alert_mqtt */
    struct sensor_data d1 = make_data(50.0, 55.0, 1013.0);
    uint8_t act = anomaly_engine_evaluate(&d1, NULL, 0);
    assert(act & ACTION_ALERT_MQTT);
    assert(!(act & ACTION_GPIO_1));
    printf("  alert_mqtt only:              PASS\n");

    /* 湿度和温度都异常 → alert_mqtt + gpio_1 + gpio_2 */
    struct sensor_data d2 = make_data(50.0, 5.0, 1013.0);
    act = anomaly_engine_evaluate(&d2, NULL, 0);
    assert(act & ACTION_ALERT_MQTT);
    assert(act & ACTION_GPIO_1);
    assert(act & ACTION_GPIO_2);
    printf("  mqtt + gpio_1 + gpio_2:       PASS\n");

    anomaly_engine_close();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 6: 统计数据
 * ═══════════════════════════════════════════════════════════ */
static void test_statistics(void)
{
    printf("--- test_statistics ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.anomaly_enabled = 1;
    cfg.anomaly_count   = 2;

    make_anomaly(&cfg.anoms[0], "a1", "temperature",
                 ANOMALY_ZSCORE, 2.0, ACTION_LOG_ONLY, 0);
    make_anomaly(&cfg.anoms[1], "a2", "humidity",
                 ANOMALY_ZSCORE, 2.0, ACTION_LOG_ONLY, 0);

    /* 减小窗口加速测试 */
    cfg.anoms[0].window_size = 10;
    cfg.anoms[1].window_size = 10;

    assert(anomaly_engine_init(&cfg) == E_OK);

    /* 建立基线 */
    for (int i = 0; i < 10; i++) {
        struct sensor_data d = make_data(25.0, 55.0, 1013.0);
        anomaly_engine_evaluate(&d, NULL, 0);
    }

    /* a1 触发 3 次，a2 触发 1 次 */
    for (int i = 0; i < 3; i++) {
        struct sensor_data d = make_data(50.0, 10.0, 1013.0);
        anomaly_engine_evaluate(&d, NULL, 0);
    }

    struct anomaly_stats stats[4];
    int n = anomaly_engine_get_stats(stats, 4);
    assert(n == 2);
    assert(stats[0].trigger_count >= 1);  /* a1 至少触发 1 次 */
    assert(stats[1].trigger_count >= 1);  /* a2 至少触发 1 次 */
    printf("  a1 count=%d, a2 count=%d:    PASS\n",
           stats[0].trigger_count, stats[1].trigger_count);

    anomaly_engine_close();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 7: 配置解析（通过 config_load）
 * ═══════════════════════════════════════════════════════════ */
static void test_config_parsing(void)
{
    printf("--- test_config_parsing ---\n");

    const char *tmp_path = "test_anomaly_tmp.conf";
    const char *content =
        "broker_host = 127.0.0.1\n"
        "broker_port = 1883\n"
        "anomaly_enabled = 1\n"
        "anomaly_1 = temperature,zscore,3.0,alert_mqtt+gpio_1\n"
        "anomaly_2 = humidity,zscore,2.5,alert_mqtt\n"
        "anomaly_3 = pressure,iforest,0.65,log_only\n";

    FILE *fp = fopen(tmp_path, "w");
    assert(fp);
    fprintf(fp, "%s", content);
    fclose(fp);

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    assert(config_load(tmp_path, &cfg) == E_OK);

    assert(cfg.anomaly_enabled == 1);
    assert(cfg.anomaly_count == 3);

    /* anomaly_1 */
    assert(strcmp(cfg.anoms[0].name, "anomaly_1") == 0);
    assert(strcmp(cfg.anoms[0].field, "temperature") == 0);
    assert(cfg.anoms[0].algo == ANOMALY_ZSCORE);
    assert(cfg.anoms[0].zscore_threshold == 3.0);
    assert(cfg.anoms[0].action_mask == (ACTION_ALERT_MQTT | ACTION_GPIO_1));

    /* anomaly_2 */
    assert(strcmp(cfg.anoms[1].field, "humidity") == 0);
    assert(cfg.anoms[1].algo == ANOMALY_ZSCORE);
    assert(cfg.anoms[1].zscore_threshold == 2.5);
    assert(cfg.anoms[1].action_mask == ACTION_ALERT_MQTT);

    /* anomaly_3 — iforest */
    assert(strcmp(cfg.anoms[2].field, "pressure") == 0);
    assert(cfg.anoms[2].algo == ANOMALY_IFOREST);
    assert(cfg.anoms[2].iforest_enabled == 1);
    assert(cfg.anoms[2].zscore_threshold == 0.65);
    assert(cfg.anoms[2].action_mask == ACTION_LOG_ONLY);

    printf("  parsed 3 anomaly rules:       PASS\n");

    /* 用解析出来的配置初始化引擎并验证 */
    assert(anomaly_engine_init(&cfg) == E_OK);

    /* 建立基线后触发 zscore anomaly_1 */
    for (int i = 0; i < 10; i++) {
        struct sensor_data d = make_data(25.0, 55.0, 1013.0);
        anomaly_engine_evaluate(&d, NULL, 0);
    }
    struct sensor_data d_bad = make_data(60.0, 55.0, 1013.0);
    uint8_t act = anomaly_engine_evaluate(&d_bad, NULL, 0);
    assert(act & ACTION_ALERT_MQTT);
    assert(act & ACTION_GPIO_1);
    printf("  evaluated from parsed config: PASS\n");

    anomaly_engine_close();
    remove(tmp_path);
}

/* ═══════════════════════════════════════════════════════════
 * 测试 8: iForest 模型（stub / 真实模型均可）
 * ═══════════════════════════════════════════════════════════ */
static void test_iforest_model(void)
{
    printf("--- test_iforest_model ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.anomaly_enabled = 1;
    cfg.anomaly_count   = 1;

    make_anomaly(&cfg.anoms[0], "a_if", "pressure",
                 ANOMALY_IFOREST, 0.65, ACTION_ALERT_MQTT, 0);

    assert(anomaly_engine_init(&cfg) == E_OK);

    /* 用正常范围的传感器数据评估 */
    struct sensor_data d_normal = make_data(25.0, 55.0, 1013.0);
    uint8_t act = anomaly_engine_evaluate(&d_normal, NULL, 0);

    /* 行为取决于模型是否可用 */
    if (IFOREST_AVAILABLE) {
        /* 真实模型已加载 → 正常数据应得分 < 阈值，不触发 */
        printf("  iforest model loaded (%d trees)\n", IFOREST_N_TREES);
        /* 正常数据通常在 0.3~0.5 分，低于 0.65 阈值 */
        assert(act == 0);
        printf("  normal sample not triggered:  PASS\n");

        /* 极端异常数据应得分 > 阈值 */
        struct sensor_data d_anom = make_data(42.0, 15.0, 985.0);
        act = anomaly_engine_evaluate(&d_anom, NULL, 0);
        /* 注意：stub 模型下 act=0，真实模型下通常触发 */
        printf("  extreme sample: act=0x%02x\n", act);
    } else {
        /* Stub 模型 → 返回 -1，永不触发 */
        printf("  iforest model not available (stub)\n");
        assert(act == 0);
        printf("  stub gracefully returns no-alert: PASS\n");
    }

    anomaly_engine_close();
}

/* ═══════════════════════════════════════════════════════════ */

int main(void)
{
    printf("=== Anomaly Engine Unit Tests ===\n\n");

    test_zscore_basic();
    test_window_sliding();
    test_cooldown();
    test_edge_cases();
    test_action_masks();
    test_statistics();
    test_config_parsing();
    test_iforest_model();

    printf("\n=== ALL anomaly engine tests PASSED ===\n");
    return 0;
}
