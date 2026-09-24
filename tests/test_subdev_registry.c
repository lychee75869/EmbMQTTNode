/*
 * tests/test_subdev_registry.c
 * 子设备注册表解析 + 校验单元测试（v1.3.0 T01，docs/12 §5）
 *
 * 覆盖（fail-closed 校验，非法整条丢弃 + WARN）:
 *   - 合法多条目（modbus、service_id 缺省/显式、注释/段跳过）
 *   - 条数 > SUBDEVICE_MAX(16) → 超出丢弃
 *   - 非法 data_source（ble / sensor——v1.4.0 网关纯化后 sensor 通道
 *     移除，sensor 条目落入 unknown data_source 的 WARN+丢弃兜底）
 *   - modbus slave_id 越界(0/248/非数字)
 *   - device_id 超长(257) / 含通配符($ # + 空格) / 重复
 *   - (data_source, source_key) 重复
 *   - service_id 非法字符回落缺省 SensorData
 *   - 空文件 / 文件不存在 → 0 条不崩
 *   - 查询接口 subdev_find_modbus（含未命中 NULL）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/subdev_registry.h"

#define TMP_CONF "test_subdev_tmp.conf"

static void write_conf(const char *content)
{
    FILE *fp = fopen(TMP_CONF, "w");
    assert(fp != NULL);
    fputs(content, fp);
    fclose(fp);
}

/* ═══════════════════════════════════════════════════════════
 * 1. 合法多条目 + 查询 + out 数组回填
 * ═══════════════════════════════════════════════════════════ */
static void test_valid_entries(void)
{
    printf("--- test_valid_entries ---\n");

    write_conf(
        "# comment line\n"
        "; another comment\n"
        "[subdevices]\n"
        "subdevice_1 = modbus,1,dev-plc1,PLC-Line1\n"
        "subdevice_2 = modbus,2,dev-plc2,PLC-Line2,SensorData\n"
        "subdevice_3 = modbus,5,dev-plc5,PLC-Line5,CustomSvc\n"
        "subdevice_4 = modbus,9,dev-plc9,PLC-Line9\n");

    struct subdev_entry out[SUBDEVICE_MAX];
    int n = subdev_load(TMP_CONF, out, SUBDEVICE_MAX);
    assert(n == 4);

    /* out 数组回填顺序 = 出现顺序 */
    assert(out[0].src == SUBDEV_MODBUS);
    assert(out[0].slave_id == 1);
    assert(strcmp(out[0].device_id, "dev-plc1") == 0);
    assert(strcmp(out[0].name, "PLC-Line1") == 0);
    assert(strcmp(out[0].service_id, "SensorData") == 0);  /* 缺省 */

    assert(out[3].src == SUBDEV_MODBUS);
    assert(out[3].slave_id == 9);
    assert(strcmp(out[3].device_id, "dev-plc9") == 0);
    assert(strcmp(out[3].service_id, "SensorData") == 0);

    /* 查询接口 */
    const struct subdev_entry *e = subdev_find_modbus(1);
    assert(e != NULL && strcmp(e->device_id, "dev-plc1") == 0);
    e = subdev_find_modbus(2);
    assert(e != NULL && strcmp(e->device_id, "dev-plc2") == 0);
    assert(strcmp(e->service_id, "SensorData") == 0);      /* 显式缺省值 */
    e = subdev_find_modbus(5);
    assert(e != NULL && strcmp(e->device_id, "dev-plc5") == 0);
    assert(strcmp(e->service_id, "CustomSvc") == 0);       /* 显式自定义 */
    /* 未命中 */
    assert(subdev_find_modbus(99) == NULL);
    assert(subdev_find_modbus(0) == NULL);

    printf("  valid 4 entries + queries: PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 2. 条数 > SUBDEVICE_MAX(16) → 超出丢弃
 * ═══════════════════════════════════════════════════════════ */
static void test_too_many(void)
{
    printf("--- test_too_many ---\n");

    FILE *fp = fopen(TMP_CONF, "w");
    assert(fp != NULL);
    /* 17 条合法 modbus 条目（slave_id 1..17，device_id 唯一） */
    for (int i = 1; i <= 17; i++)
        fprintf(fp, "subdevice_%d = modbus,%d,dev-%d,PLC-%d\n",
                i, i, i, i);
    fclose(fp);

    int n = subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX);
    assert(n == SUBDEVICE_MAX);          /* 第 17 条被丢弃 */
    assert(subdev_find_modbus(16) != NULL);   /* 第 16 条保留 */
    assert(subdev_find_modbus(17) == NULL);   /* 第 17 条丢弃 */
    printf("  17 entries -> %d loaded (cap 16): PASS\n", n);
}

/* ═══════════════════════════════════════════════════════════
 * 3. 非法 data_source（ble / sensorx / sensor）
 * ═══════════════════════════════════════════════════════════ */
static void test_invalid_source(void)
{
    printf("--- test_invalid_source ---\n");

    /* 预留 ble 及未知 data_source → 丢弃 */
    write_conf(
        "subdevice_1 = ble,foo,dev-ble,BLE\n"
        "subdevice_2 = sensorx,sht30,dev-bad,Bad\n");
    assert(subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX) == 0);
    printf("  invalid data_source dropped: PASS\n");

    /* v1.4.0 网关纯化：sensor 通道移除——原 sensor 条目一律落入
     * unknown data_source 的 WARN+丢弃兜底（含原合法 sensor_type） */
    write_conf(
        "subdevice_1 = sensor,sht30,dev-ok,OK\n"
        "subdevice_2 = sensor,co2,dev-co2,CO2\n");
    assert(subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX) == 0);
    assert(subdev_count() == 0);
    printf("  sensor channel removed -> entries dropped: PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 4. modbus slave_id 越界 / 非数字
 * ═══════════════════════════════════════════════════════════ */
static void test_modbus_slave_range(void)
{
    printf("--- test_modbus_slave_range ---\n");

    write_conf(
        "subdevice_1 = modbus,0,dev-0,Bad0\n"
        "subdevice_2 = modbus,248,dev-248,Bad248\n"
        "subdevice_3 = modbus,abc,dev-abc,BadAbc\n"
        "subdevice_4 = modbus,247,dev-247,OkMax\n"
        "subdevice_5 = modbus,1,dev-1,OkMin\n");
    int n = subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX);
    assert(n == 2);
    assert(subdev_find_modbus(247) != NULL);
    assert(subdev_find_modbus(1) != NULL);
    assert(subdev_find_modbus(0) == NULL);
    assert(subdev_find_modbus(248) == NULL);
    printf("  slave_id range 1-247 enforced: PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 5. device_id 长度边界（256 合法 / 257 非法）
 * ═══════════════════════════════════════════════════════════ */
static void test_device_id_length(void)
{
    printf("--- test_device_id_length ---\n");

    char big256[257];
    char big257[258];
    memset(big256, 'a', 256); big256[256] = '\0';
    memset(big257, 'b', 257); big257[257] = '\0';

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "subdevice_1 = modbus,1,%s,L256\n"
             "subdevice_2 = modbus,2,%s,L257\n",
             big256, big257);
    write_conf(buf);

    int n = subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX);
    assert(n == 1);                          /* 256 保留，257 丢弃 */
    assert(subdev_find_modbus(1) != NULL);
    assert(subdev_find_modbus(2) == NULL);
    printf("  device_id len 256 ok / 257 dropped: PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 6. device_id 含通配符/空格
 * ═══════════════════════════════════════════════════════════ */
static void test_device_id_illegal_chars(void)
{
    printf("--- test_device_id_illegal_chars ---\n");

    /* 非法字符条目在 dup 检查前即被丢弃，唯一合法条目不受影响 */
    write_conf(
        "subdevice_1 = modbus,1,dev$bad,S\n"
        "subdevice_2 = modbus,1,dev#bad,S\n"
        "subdevice_3 = modbus,1,dev+bad,S\n"
        "subdevice_4 = modbus,1,dev bad,S\n"
        "subdevice_5 = modbus,1,dev-ok,S\n");
    int n = subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX);
    assert(n == 1);
    assert(subdev_find_modbus(1) != NULL);
    assert(strcmp(subdev_find_modbus(1)->device_id, "dev-ok") == 0);
    printf("  device_id wildcard/space rejected: PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 7. device_id 重复 / (src,key) 重复
 * ═══════════════════════════════════════════════════════════ */
static void test_duplicates(void)
{
    printf("--- test_duplicates ---\n");

    /* device_id 全表唯一 */
    write_conf(
        "subdevice_1 = modbus,1,dup-id,First\n"
        "subdevice_2 = modbus,2,dup-id,Second\n");
    assert(subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX) == 1);
    printf("  duplicate device_id dropped: PASS\n");

    /* (data_source, source_key) 全表唯一：两条 modbus,1 */
    write_conf(
        "subdevice_1 = modbus,1,id-a,A\n"
        "subdevice_2 = modbus,1,id-b,B\n");
    assert(subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX) == 1);
    printf("  duplicate (modbus,1) dropped: PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 8. service_id 非法字符 → 回落缺省（仍加载）
 * ═══════════════════════════════════════════════════════════ */
static void test_service_id_fallback(void)
{
    printf("--- test_service_id_fallback ---\n");

    write_conf("subdevice_1 = modbus,1,dev-ok,Svc,Bad$Svc\n");
    int n = subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX);
    assert(n == 1);
    const struct subdev_entry *e = subdev_find_modbus(1);
    assert(e != NULL);
    assert(strcmp(e->service_id, "SensorData") == 0);
    printf("  illegal service_id -> SensorData: PASS\n");
}

/* ═══════════════════════════════════════════════════════════
 * 9. 空文件 / 文件不存在 → 0 条不崩
 * ═══════════════════════════════════════════════════════════ */
static void test_empty_and_missing(void)
{
    printf("--- test_empty_and_missing ---\n");

    write_conf("# only comments\n; nothing\n[empty]\n");
    assert(subdev_load(TMP_CONF, NULL, SUBDEVICE_MAX) == 0);

    assert(subdev_load("no_such_file_xyz.conf", NULL, SUBDEVICE_MAX) == 0);
    assert(subdev_find_modbus(1) == NULL);   /* 空表 */
    printf("  empty/missing file -> 0 (no crash): PASS\n");
}

/* ═══════════════════════════════════════════════════════════ */

int main(void)
{
    printf("=== Subdev Registry Unit Tests ===\n\n");

    test_valid_entries();
    test_too_many();
    test_invalid_source();
    test_modbus_slave_range();
    test_device_id_length();
    test_device_id_illegal_chars();
    test_duplicates();
    test_service_id_fallback();
    test_empty_and_missing();

    remove(TMP_CONF);
    printf("\n=== ALL subdev registry tests PASSED ===\n");
    return 0;
}
