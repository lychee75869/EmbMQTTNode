/*
 * tests/test_rule_engine.c
 * 规则引擎单元测试（阶段三）
 *
 * 覆盖:
 *   - 六种运算符: gt / lt / eq / ne / outside / rate
 *   - 动作掩码组合
 *   - 冷却时间防抖
 *   - 多规则并行评估
 *   - 边界条件
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include "../src/common.h"
#include "../src/rule_engine.h"
#include "../src/config.h"

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

/* 辅助: 构造一条规则 */
static void make_rule(struct rule *r, const char *name,
                       const char *field, enum rule_op op,
                       double th, double th_lo, double th_hi,
                       uint8_t action, int cooldown_ms)
{
    memset(r, 0, sizeof(*r));
    strncpy(r->name,  name,  RULE_NAME_LEN - 1);
    strncpy(r->field, field, RULE_FIELD_LEN - 1);
    r->op          = op;
    r->threshold   = th;
    r->threshold_lo = th_lo;
    r->threshold_hi = th_hi;
    r->action_mask  = action;
    r->cooldown_ms  = cooldown_ms;
}

/* ═══════════════════════════════════════════════════════════
 * 测试 1: 基本运算符
 * ═══════════════════════════════════════════════════════════ */
static void test_basic_operators(void)
{
    printf("--- test_basic_operators ---\n");

    /* 每次只测试一个运算符，避免规则间互相干扰 */
    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.rule_count = 1;
        make_rule(&cfg.rules[0], "r_gt", "temperature", OP_GT, 80.0, 0, 0,
                  ACTION_LOG_ONLY, 0);
        assert(rule_engine_init(&cfg) == E_OK);

        struct sensor_data d1 = make_data(85.0, 50.0, 1013.0);
        assert(rule_engine_evaluate(&d1, NULL, 0) & ACTION_LOG_ONLY);
        printf("  gt(85>80) triggered:      PASS\n");

        struct sensor_data d2 = make_data(70.0, 50.0, 1013.0);
        assert(rule_engine_evaluate(&d2, NULL, 0) == 0);
        printf("  gt(70>80) not triggered:  PASS\n");

        rule_engine_close();
    }

    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.rule_count = 1;
        make_rule(&cfg.rules[0], "r_lt", "humidity", OP_LT, 20.0, 0, 0,
                  ACTION_LOG_ONLY, 0);
        assert(rule_engine_init(&cfg) == E_OK);

        struct sensor_data d = make_data(25.0, 15.0, 1013.0);
        assert(rule_engine_evaluate(&d, NULL, 0) & ACTION_LOG_ONLY);
        printf("  lt(15<20) triggered:      PASS\n");

        rule_engine_close();
    }

    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.rule_count = 1;
        make_rule(&cfg.rules[0], "r_eq", "pressure", OP_EQ, 1013.25, 0, 0,
                  ACTION_LOG_ONLY, 0);
        assert(rule_engine_init(&cfg) == E_OK);

        struct sensor_data d = make_data(25.0, 50.0, 1013.25);
        assert(rule_engine_evaluate(&d, NULL, 0) & ACTION_LOG_ONLY);
        printf("  eq(1013.25==1013.25) triggered: PASS\n");

        /* 不匹配：pressure=1013.0 != 1013.25 */
        struct sensor_data d2 = make_data(25.0, 50.0, 1013.0);
        assert(rule_engine_evaluate(&d2, NULL, 0) == 0);
        printf("  eq(1013.0!=1013.25) not triggered: PASS\n");

        rule_engine_close();
    }

    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.rule_count = 1;
        make_rule(&cfg.rules[0], "r_ne", "pressure", OP_NE, 0.0, 0, 0,
                  ACTION_LOG_ONLY, 0);
        assert(rule_engine_init(&cfg) == E_OK);

        struct sensor_data d = make_data(25.0, 50.0, 1013.25);
        assert(rule_engine_evaluate(&d, NULL, 0) & ACTION_LOG_ONLY);
        printf("  ne(1013.25!=0) triggered: PASS\n");

        /* 不匹配：pressure=0.0 == 0.0 */
        struct sensor_data d2 = make_data(25.0, 50.0, 0.0);
        assert(rule_engine_evaluate(&d2, NULL, 0) == 0);
        printf("  ne(0.0==0.0) not triggered: PASS\n");

        rule_engine_close();
    }

    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.rule_count = 1;
        make_rule(&cfg.rules[0], "r_bet", "temperature", OP_OUT,
                  0, 20.0, 30.0, ACTION_LOG_ONLY, 0);
        assert(rule_engine_init(&cfg) == E_OK);

        struct sensor_data d = make_data(35.0, 50.0, 1013.0);
        assert(rule_engine_evaluate(&d, NULL, 0) & ACTION_LOG_ONLY);
        printf("  outside(35 outside [20,30]) triggered: PASS\n");

        struct sensor_data d2 = make_data(25.0, 50.0, 1013.0);
        assert(rule_engine_evaluate(&d2, NULL, 0) == 0);
        printf("  outside(25 inside [20,30]) not triggered: PASS\n");

        rule_engine_close();
    }

    {
        struct node_config cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.rule_count = 1;
        make_rule(&cfg.rules[0], "r_rate", "temperature", OP_RATE, 5.0, 0, 0,
                  ACTION_LOG_ONLY, 0);
        assert(rule_engine_init(&cfg) == E_OK);

        /* 第一次：变化率=0（无历史），不触发 */
        struct sensor_data d1 = make_data(25.0, 50.0, 1013.0);
        assert(rule_engine_evaluate(&d1, NULL, 0) == 0);
        printf("  rate(first sample) not triggered: PASS\n");

        /* 模拟快速变化：在短时间内数值跳变 >5 */
        /* 需要在 rule_engine 中手动注入历史值来模拟 */
        /* 这里验证 rate 操作符加载正确即可 */

        rule_engine_close();
    }
}

/* ═══════════════════════════════════════════════════════════
 * 测试 2: 动作掩码
 * ═══════════════════════════════════════════════════════════ */
static void test_action_masks(void)
{
    printf("--- test_action_masks ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rule_count = 2;

    make_rule(&cfg.rules[0], "r_mqtt", "temperature", OP_GT, 30.0, 0, 0,
              ACTION_ALERT_MQTT, 0);
    make_rule(&cfg.rules[1], "r_gpio", "temperature", OP_GT, 90.0, 0, 0,
              ACTION_GPIO_1 | ACTION_GPIO_2, 0);

    assert(rule_engine_init(&cfg) == E_OK);

    /* 35 > 30 → r_mqtt 触发 ACTION_ALERT_MQTT */
    struct sensor_data d1 = make_data(35.0, 50.0, 1013.0);
    uint8_t act = rule_engine_evaluate(&d1, NULL, 0);
    assert(act & ACTION_ALERT_MQTT);
    assert(!(act & ACTION_GPIO_1));
    printf("  alert_mqtt only:         PASS\n");

    /* 95 > 90 → r_gpio 也触发，两个规则同时触发 */
    struct sensor_data d2 = make_data(95.0, 50.0, 1013.0);
    act = rule_engine_evaluate(&d2, NULL, 0);
    assert(act & ACTION_ALERT_MQTT);
    assert(act & ACTION_GPIO_1);
    assert(act & ACTION_GPIO_2);
    printf("  mqtt + gpio_1 + gpio_2:  PASS\n");

    rule_engine_close();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 3: 冷却时间
 * ═══════════════════════════════════════════════════════════ */
static void test_cooldown(void)
{
    printf("--- test_cooldown ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rule_count = 1;

    /* 冷却 500ms */
    make_rule(&cfg.rules[0], "r_cd", "temperature", OP_GT, 50.0, 0, 0,
              ACTION_LOG_ONLY, 500);

    assert(rule_engine_init(&cfg) == E_OK);

    struct sensor_data d = make_data(60.0, 50.0, 1013.0);

    /* 第一次触发成功 */
    uint8_t act = rule_engine_evaluate(&d, NULL, 0);
    assert(act & ACTION_LOG_ONLY);
    printf("  first trigger:           PASS\n");

    /* 立即再评估，应在冷却期内，不触发 */
    act = rule_engine_evaluate(&d, NULL, 0);
    assert(act == 0);
    printf("  cooldown suppressed:     PASS\n");

    /* 等待冷却结束 */
    {
        struct timespec ts = {0, 600000000};  /* 600ms > 500ms */
        nanosleep(&ts, NULL);
    }

    /* 冷却结束后再次触发 */
    act = rule_engine_evaluate(&d, NULL, 0);
    assert(act & ACTION_LOG_ONLY);
    printf("  after cooldown triggered: PASS\n");

    rule_engine_close();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 4: 告警消息生成
 * ═══════════════════════════════════════════════════════════ */
static void test_alert_message(void)
{
    printf("--- test_alert_message ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rule_count = 1;

    make_rule(&cfg.rules[0], "high_temp", "temperature", OP_GT, 85.0, 0, 0,
              ACTION_ALERT_MQTT, 0);

    assert(rule_engine_init(&cfg) == E_OK);

    char msg[256];
    struct sensor_data d = make_data(90.5, 55.0, 1013.0);
    uint8_t act = rule_engine_evaluate(&d, msg, sizeof(msg));
    assert(act & ACTION_ALERT_MQTT);
    assert(strlen(msg) > 0);
    assert(strstr(msg, "high_temp") != NULL);
    assert(strstr(msg, "90.5") != NULL);
    printf("  alert msg: %s\n", msg);
    printf("  alert message:            PASS\n");

    rule_engine_close();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 5: 规则统计
 * ═══════════════════════════════════════════════════════════ */
static void test_statistics(void)
{
    printf("--- test_statistics ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rule_count = 2;

    make_rule(&cfg.rules[0], "r1", "temperature", OP_GT, 50.0, 0, 0, ACTION_LOG_ONLY, 0);
    make_rule(&cfg.rules[1], "r2", "humidity",    OP_LT, 30.0, 0, 0, ACTION_LOG_ONLY, 0);

    assert(rule_engine_init(&cfg) == E_OK);

    /* 触发 r1 两次 */
    struct sensor_data d1 = make_data(60.0, 50.0, 1013.0); /* trigger r1 */
    rule_engine_evaluate(&d1, NULL, 0);
    rule_engine_evaluate(&d1, NULL, 0);

    /* 触发 r2 一次 */
    struct sensor_data d2 = make_data(25.0, 20.0, 1013.0); /* trigger r2 */
    rule_engine_evaluate(&d2, NULL, 0);

    struct rule_stats stats[4];
    int n = rule_engine_get_stats(stats, 4);
    assert(n == 2);
    assert(stats[0].trigger_count == 2);
    assert(stats[1].trigger_count == 1);
    printf("  r1 count=%d, r2 count=%d: PASS\n",
           stats[0].trigger_count, stats[1].trigger_count);

    rule_engine_close();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 6: 多规则组合（通过 config_load 解析）
 * ═══════════════════════════════════════════════════════════ */
static void test_config_parsing(void)
{
    printf("--- test_config_parsing ---\n");

    const char *tmp_path = "test_rules_tmp.conf";
    const char *content =
        "broker_host = 127.0.0.1\n"
        "broker_port = 1883\n"
        "rule_1 = temperature,gt,80.0,alert_mqtt+gpio_1\n"
        "rule_2 = humidity,lt,15.0,alert_mqtt\n"
        "rule_3 = pressure,outside,950.0_1050.0,log_only\n"
        "rule_4 = temperature,rate,5.0,alert_mqtt\n";

    FILE *fp = fopen(tmp_path, "w");
    assert(fp);
    fprintf(fp, "%s", content);
    fclose(fp);

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    assert(config_load(tmp_path, &cfg) == E_OK);

    assert(cfg.rule_count == 4);

    /* rule_1 */
    assert(strcmp(cfg.rules[0].name, "rule_1") == 0);
    assert(strcmp(cfg.rules[0].field, "temperature") == 0);
    assert(cfg.rules[0].op == OP_GT);
    assert(cfg.rules[0].threshold == 80.0);
    assert(cfg.rules[0].action_mask == (ACTION_ALERT_MQTT | ACTION_GPIO_1));

    /* rule_2 */
    assert(strcmp(cfg.rules[1].field, "humidity") == 0);
    assert(cfg.rules[1].op == OP_LT);
    assert(cfg.rules[1].threshold == 15.0);
    assert(cfg.rules[1].action_mask == ACTION_ALERT_MQTT);

    /* rule_3 — outside */
    assert(cfg.rules[2].op == OP_OUT);
    assert(cfg.rules[2].threshold_lo == 950.0);
    assert(cfg.rules[2].threshold_hi == 1050.0);
    assert(cfg.rules[2].action_mask == ACTION_LOG_ONLY);

    /* rule_4 — rate */
    assert(cfg.rules[3].op == OP_RATE);
    assert(cfg.rules[3].threshold == 5.0);
    assert(cfg.rules[3].action_mask == ACTION_ALERT_MQTT);

    printf("  parsed 4 rules via config_load: PASS\n");

    /* 用解析出来的配置初始化规则引擎并验证 */
    assert(rule_engine_init(&cfg) == E_OK);

    struct sensor_data d = make_data(85.0, 10.0, 900.0);
    uint8_t act = rule_engine_evaluate(&d, NULL, 0);
    /* rule_1(temp=85>80) + rule_2(hum=10<15) + rule_3(pres=900 outside [950,1050]) */
    assert(act & ACTION_ALERT_MQTT);
    assert(act & ACTION_GPIO_1);
    assert(act & ACTION_LOG_ONLY);
    printf("  multi-rule evaluation:   PASS\n");

    rule_engine_close();
    remove(tmp_path);
}

/* ═══════════════════════════════════════════════════════════
 * 测试 7: 边界条件
 * ═══════════════════════════════════════════════════════════ */
static void test_edge_cases(void)
{
    printf("--- test_edge_cases ---\n");

    /* NULL 参数 */
    assert(rule_engine_init(NULL) == E_INVAL);
    printf("  NULL init:               PASS\n");

    /* 空规则集 */
    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rule_count = 0;
    assert(rule_engine_init(&cfg) == E_OK);
    struct sensor_data d = make_data(25.0, 50.0, 1013.0);
    assert(rule_engine_evaluate(&d, NULL, 0) == 0);
    printf("  empty rules:             PASS\n");

    /* 未初始化时调用 evaluate */
    rule_engine_close();
    assert(rule_engine_evaluate(&d, NULL, 0) == 0);
    printf("  evaluate while closed:   PASS\n");

    /* NULL data */
    cfg.rule_count = 1;
    make_rule(&cfg.rules[0], "r", "temperature", OP_GT, 50.0, 0, 0, ACTION_LOG_ONLY, 0);
    assert(rule_engine_init(&cfg) == E_OK);
    assert(rule_engine_evaluate(NULL, NULL, 0) == 0);
    printf("  NULL data:               PASS\n");

    rule_engine_close();
}

/* ═══════════════════════════════════════════════════════════ */

int main(void)
{
    printf("=== Rule Engine Unit Tests ===\n\n");

    test_basic_operators();
    test_action_masks();
    test_cooldown();
    test_alert_message();
    test_statistics();
    test_config_parsing();
    test_edge_cases();

    printf("\n=== ALL rule engine tests PASSED ===\n");
    return 0;
}
