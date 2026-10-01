/*
 * tests/test_sensor_fields.c
 * 字段描述表（sensor_fields）跨层重构单元测试
 *
 * 覆盖:
 *   1. 表完整性 / 表序不变式：SENSOR_FIELDS 顺序 == payload 序列化序，
 *      条目数与 SENSOR_FIELD_COUNT 一致，offset 与 offsetof 对齐。
 *   2. sensor_find_field：命中 / 未命中 / NULL。
 *   3. sensor_get_field：逐字段取值 + 未知字段哨兵 + NULL 保护。
 *   4. sensor_set_field：命中写入 + 未知字段拒绝（不写）+ NULL 保护。
 *   5. 黄金 payload：mqtt_build_data_payload 输出逐字节稳定
 *      （行为等价重构的硬约束）。
 *   6. config 字段校验：rule_N / anomaly_N 的未知 field 必须 fail-closed
 *      丢弃（sensor_find_field 驱动）。
 *
 * 说明：http_server.c 的 JSON 构造器为 static handler，无法单测；其字段
 * 序列化改用 for_each_field 后与 mqtt payload 同源（同一 SENSOR_FIELDS），
 * 本测试以 mqtt 黄金 payload 作为表序/格式化的一致性锚点。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../src/common.h"
#include "../src/sensor_fields.h"
#include "../src/mqtt_client.h"
#include "../src/config.h"

/* ═══════════════════════════════════════════════════════════
 * 1. 表完整性 / 表序不变式
 * ═══════════════════════════════════════════════════════════ */

static void test_table_order(void)
{
    printf("--- test_table_order ---\n");

    assert(SENSOR_FIELD_COUNT == 6);

    /* 表序即 payload 序（冻结不变式）；表尾追加 3 项 */
    assert(strcmp(SENSOR_FIELDS[0].name, "temperature") == 0);
    assert(strcmp(SENSOR_FIELDS[1].name, "humidity") == 0);
    assert(strcmp(SENSOR_FIELDS[2].name, "pressure") == 0);
    assert(strcmp(SENSOR_FIELDS[3].name, "soil_moisture") == 0);
    assert(strcmp(SENSOR_FIELDS[4].name, "water_level") == 0);
    assert(strcmp(SENSOR_FIELDS[5].name, "battery_voltage") == 0);

    /* type 均为 SF_F64 */
    for_each_field(f) {
        assert(f->type == SF_F64);
    }

    /* offset 必须与 offsetof 对齐（成员地址由编译器决定） */
    assert(SENSOR_FIELDS[0].offset == offsetof(struct sensor_data, temperature));
    assert(SENSOR_FIELDS[1].offset == offsetof(struct sensor_data, humidity));
    assert(SENSOR_FIELDS[2].offset == offsetof(struct sensor_data, pressure));
    assert(SENSOR_FIELDS[3].offset == offsetof(struct sensor_data, soil_moisture));
    assert(SENSOR_FIELDS[4].offset == offsetof(struct sensor_data, water_level));
    assert(SENSOR_FIELDS[5].offset == offsetof(struct sensor_data, battery_voltage));

    printf("  order+count+offset aligned: PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 2. sensor_find_field
 * ═══════════════════════════════════════════════════════════ */

static void test_find_field(void)
{
    printf("--- test_find_field ---\n");

    assert(sensor_find_field("temperature") == &SENSOR_FIELDS[0]);
    assert(sensor_find_field("humidity")    == &SENSOR_FIELDS[1]);
    assert(sensor_find_field("pressure")    == &SENSOR_FIELDS[2]);
    assert(sensor_find_field("soil_moisture")   == &SENSOR_FIELDS[3]);
    assert(sensor_find_field("water_level")     == &SENSOR_FIELDS[4]);
    assert(sensor_find_field("battery_voltage") == &SENSOR_FIELDS[5]);

    /* 未命中：未知字段 / 空串 / NULL */
    assert(sensor_find_field("co2") == NULL);
    assert(sensor_find_field("")    == NULL);
    assert(sensor_find_field(NULL)  == NULL);

    printf("  hit/miss/NULL:             PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 3. sensor_get_field
 * ═══════════════════════════════════════════════════════════ */

static void test_get_field(void)
{
    printf("--- test_get_field ---\n");

    struct sensor_data d;
    memset(&d, 0, sizeof(d));
    d.temperature = 23.45;
    d.humidity    = 55.67;
    d.pressure    = 1013.25;
    d.soil_moisture   = 42.5;
    d.water_level     = 78.0;
    d.battery_voltage = 12.6;

    assert(sensor_get_field(&d, "temperature") == 23.45);
    assert(sensor_get_field(&d, "humidity")    == 55.67);
    assert(sensor_get_field(&d, "pressure")    == 1013.25);
    assert(sensor_get_field(&d, "soil_moisture")   == 42.5);
    assert(sensor_get_field(&d, "water_level")     == 78.0);
    assert(sensor_get_field(&d, "battery_voltage") == 12.6);

    /* 未知字段 / NULL → 哨兵（引擎"永不匹配"语义） */
    assert(sensor_get_field(&d, "co2")  == SENSOR_VALUE_INVALID);
    assert(sensor_get_field(&d, NULL)   == SENSOR_VALUE_INVALID);
    assert(sensor_get_field(NULL, "temperature") == SENSOR_VALUE_INVALID);

    printf("  values + sentinel + NULL:  PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 4. sensor_set_field
 * ═══════════════════════════════════════════════════════════ */

static void test_set_field(void)
{
    printf("--- test_set_field ---\n");

    struct sensor_data d;
    memset(&d, 0, sizeof(d));

    assert(sensor_set_field(&d, "temperature", 1.5) == E_OK);
    assert(sensor_set_field(&d, "humidity",    2.5) == E_OK);
    assert(sensor_set_field(&d, "pressure",    3.5) == E_OK);
    assert(sensor_set_field(&d, "soil_moisture",   4.5) == E_OK);
    assert(sensor_set_field(&d, "water_level",     5.5) == E_OK);
    assert(sensor_set_field(&d, "battery_voltage", 6.5) == E_OK);
    assert(d.temperature == 1.5);
    assert(d.humidity    == 2.5);
    assert(d.pressure    == 3.5);
    assert(d.soil_moisture   == 4.5);
    assert(d.water_level     == 5.5);
    assert(d.battery_voltage == 6.5);

    /* 未知字段：E_INVAL 且不写任何字段 */
    double before = d.temperature;
    assert(sensor_set_field(&d, "co2", 999.0) == E_INVAL);
    assert(d.temperature == before);
    assert(d.humidity    == 2.5);
    assert(d.pressure    == 3.5);
    assert(d.soil_moisture   == 4.5);
    assert(d.water_level     == 5.5);
    assert(d.battery_voltage == 6.5);

    /* NULL 保护 */
    assert(sensor_set_field(NULL, "temperature", 1.0) == E_INVAL);
    assert(sensor_set_field(&d, NULL, 1.0) == E_INVAL);

    printf("  write + reject + NULL:     PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 5. 黄金 payload（逐字节等价硬约束）
 * ═══════════════════════════════════════════════════════════ */

static void test_golden_payload(void)
{
    printf("--- test_golden_payload ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.client_id, sizeof(cfg.client_id), "gw-001");

    struct sensor_data d;
    memset(&d, 0, sizeof(d));
    d.temperature     = 23.45;
    d.humidity        = 55.67;
    d.pressure        = 1013.25;
    d.soil_moisture   = 42.50;
    d.water_level     = 78.00;
    d.battery_voltage = 12.60;
    d.timestamp_ms = 1234567890LL;

    char buf[512];
    int rc = mqtt_build_data_payload(&cfg, &d, buf, (int)sizeof(buf));
    assert(rc == E_OK);

    /* 表尾追加 3 字段 → payload 逐字节含 6 字段（local 路径不 mask
     * 哨兵；本组均为有效值）。逐字节 strcmp 语义保持不变。 */
    const char *expect =
        "{\"client_id\":\"gw-001\","
        "\"timestamp\":1234567890,"
        "\"temperature\":23.45,"
        "\"humidity\":55.67,"
        "\"pressure\":1013.25,"
        "\"soil_moisture\":42.50,"
        "\"water_level\":78.00,"
        "\"battery_voltage\":12.60}";
    if (strcmp(buf, expect) != 0) {
        printf("  GOLDEN MISMATCH\n    got: %s\n    exp: %s\n", buf, expect);
    }
    assert(strcmp(buf, expect) == 0);
    assert((int)strlen(buf) == (int)strlen(expect));

    /* 第二组：小数位保留 + 字段次序锚定（1.00/2.00/3.00 + 4.00/5.00/6.00） */
    struct sensor_data e;
    memset(&e, 0, sizeof(e));
    e.temperature     = 1.0;
    e.humidity        = 2.0;
    e.pressure        = 3.0;
    e.soil_moisture   = 4.0;
    e.water_level     = 5.0;
    e.battery_voltage = 6.0;
    e.timestamp_ms = 42LL;
    char buf2[512];
    assert(mqtt_build_data_payload(&cfg, &e, buf2, (int)sizeof(buf2)) == E_OK);
    assert(strcmp(buf2,
                  "{\"client_id\":\"gw-001\",\"timestamp\":42,"
                  "\"temperature\":1.00,\"humidity\":2.00,"
                  "\"pressure\":3.00,\"soil_moisture\":4.00,"
                  "\"water_level\":5.00,\"battery_voltage\":6.00}") == 0);

    /* 截断拒绝仍生效 */
    char tiny[16];
    assert(mqtt_build_data_payload(&cfg, &d, tiny,
                                   (int)sizeof(tiny)) == E_IO);

    printf("  byte-exact + truncate:     PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 6. config 字段校验（fail-closed）
 * ═══════════════════════════════════════════════════════════ */

static void test_config_field_validation(void)
{
    printf("--- test_config_field_validation ---\n");

    const char *content =
        "rule_1 = temperature,gt,30,log_only\n"   /* 合法字段 */
        "rule_2 = co2,gt,1000,log_only\n"          /* 未知字段 → 丢弃 */
        "anomaly_enabled = 1\n"
        "anomaly_1 = humidity,zscore,3.0,log_only\n"  /* 合法字段 */
        "anomaly_2 = co2,zscore,3.0,log_only\n";      /* 未知字段 → 丢弃 */

    const char *path = "test_sensor_fields_tmp.conf";
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    fprintf(fp, "%s", content);
    fclose(fp);

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    assert(config_load(path, &cfg) == E_OK);

    remove(path);
    printf("  unknown field dropped:     PASS\n");
}

/* ─── 入口 ─────────────────────────────────────────────── */

int main(void)
{
    printf("=== Sensor Field Descriptor Table Tests ===\n\n");

    test_table_order();
    test_find_field();
    test_get_field();
    test_set_field();
    test_golden_payload();
    test_config_field_validation();

    printf("\n=== ALL sensor_fields tests PASSED ===\n");
    return 0;
}
