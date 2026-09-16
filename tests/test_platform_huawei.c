/*
 * tests/test_platform_huawei.c
 * T03 huawei 纯函数单测——第 12 个测试（docs/12 §3.3 / §4.1 / §3.5 / §9-2/9-6）
 *
 * 覆盖：
 *   ① client_id 拼装：gmtime_r UTC、auth 0/1、buf≥320、256 边界、_0_{auth}_{ts} 格式
 *   ② password：HMAC-SHA256(key=ts,data=secret) 固定 64 小写 hex；联立用例
 *   ③ keepalive 钳制边界
 *   ④ event_time（含闰年/月末/0）
 *   ⑤ 主题拼装 + classify（CMD/REGISTER_RESP/OTHER，不误判）+ extract_request_id
 *   ⑥ payload builders：截断→E_IO、mask（-999 省略 + 全空→E_NOT_FOUND）、
 *      gateway_props/event_alert/command_response 字段
 *   ⑦ hw_connect_params：空凭据→E_INVAL；装配字段
 *
 * ── password 期望向量来源（诚实标注）──────────────────────────
 *   本文件使用两类**独立**向量，均非"自证"：
 *   (a) RFC 4231 Test Case 2（公开标准向量，绝对独立）：
 *       key="Jefe", data="what do ya want for nothing?" →
 *       5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
 *       用于验证 HMAC(key=ts,data=secret) 的参数序 + hex 编码符合标准。
 *   (b) 独立实现交叉验证（Python hashlib/hmac，独立 oracle，非本项目代码）：
 *       命令: python3 -c "import hmac,hashlib;
 *         print(hmac.new(ts.encode(),secret.encode(),hashlib.sha256).hexdigest())"
 *       A test-secret-123 / 2023091516 = beb722273e8e8f115d606e568a3087fda0c31ca5c321e14e5d650bd3b40e9107
 *       B test-secret-123 / 2023091515 = 6fc823c229b187edf5562eaa658b88213af03d024986c60504608e2d201b4611
 *       C MySecret        / 2023030916 = cc4d31455943a1140a540fd48e464a8fbac1a7dc0aac65e39b8009e06886bae7
 *   (c) **官方向量 PENDING**：离线官方文档（华为云Stack 8.5.0 开发/使用指南 PDF）
 *       未能提取出带具体数值的 password 示例（PDF 为 CID 子集字体，无可用
 *       抽取工具；zip 内仅含同批 PDF+CHM）。故**不做**"已与官方样例一致"的
 *       声称——需用户从华为 IoTDA 控制台"参数生成工具"生成 device_id/secret
 *       对应向量后再补。
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "../src/common.h"
#include "../src/platform_huawei.h"
#include "../src/sensor_fields.h"
#include "../src/subdev_registry.h"

/* ─── 工具 ───────────────────────────────────────────────── */

static int is_lower_hex64(const char *s)
{
    if (strlen(s) != 64)
        return 0;
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return 0;
    }
    return 1;
}

/* ─── ① client_id ────────────────────────────────────────── */

static void test_client_id_format(void)
{
    printf("--- test_client_id_format (T03 ①) ---\n");

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 2023 - 1900;
    tm.tm_mon  = 9 - 1;
    tm.tm_mday = 15;
    tm.tm_hour = 16;

    char buf[320];
    assert(hw_build_client_id("dev-001", 0, &tm, buf, sizeof(buf)) == E_OK);
    assert(strcmp(buf, "dev-001_0_0_2023091516") == 0);

    assert(hw_build_client_id("dev-001", 1, &tm, buf, sizeof(buf)) == E_OK);
    assert(strcmp(buf, "dev-001_0_1_2023091516") == 0);

    /* 后缀格式断言：_0_{auth}_{10 位数字} */
    const char *us = strrchr(buf, '_');
    assert(us != NULL);
    assert(strlen(us + 1) == 10);
    for (int i = 0; i < 10; i++)
        assert(us[1 + i] >= '0' && us[1 + i] <= '9');
    printf("  _0_{auth}_{YYYYMMDDHH} format: PASS\n");
}

static void test_client_id_gmtime(void)
{
    printf("--- test_client_id_gmtime (T03 ①) ---\n");

    /* 1694793600 = 2023-09-15T16:00:00Z（独立 epoch 锚点） */
    time_t epoch = (time_t)1694793600LL;
    struct tm utc;
    assert(gmtime_r(&epoch, &utc) != NULL);

    char buf[320];
    assert(hw_build_client_id("gw", 0, &utc, buf, sizeof(buf)) == E_OK);
    assert(strcmp(buf, "gw_0_0_2023091516") == 0);
    printf("  gmtime_r UTC -> 2023091516: PASS\n");
}

static void test_client_id_boundaries(void)
{
    printf("--- test_client_id_boundaries (T03 ①) ---\n");

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 2023 - 1900; tm.tm_mon = 0; tm.tm_mday = 1; tm.tm_hour = 0;

    /* device_id 长 256（官方 String(256) 上限） */
    char long_id[257];
    memset(long_id, 'a', 256);
    long_id[256] = '\0';

    char buf[320];
    assert(hw_build_client_id(long_id, 0, &tm, buf, sizeof(buf)) == E_OK);
    /* 256 + "_0_0_" (5) + "2023010100" (10) = 271 */
    assert(strlen(buf) == 271);
    assert(strncmp(buf, long_id, 256) == 0);
    printf("  256-byte device_id (len=320) OK: PASS\n");

    /* len < 320 → E_INVAL（即便内容放得下） */
    assert(hw_build_client_id("dev", 0, &tm, buf, 319) == E_INVAL);
    printf("  len<320 -> E_INVAL: PASS\n");

    /* 防御 */
    assert(hw_build_client_id(NULL, 0, &tm, buf, sizeof(buf)) == E_INVAL);
    assert(hw_build_client_id("dev", 0, NULL, buf, sizeof(buf)) == E_INVAL);
    assert(hw_build_client_id("dev", 0, &tm, NULL, sizeof(buf)) == E_INVAL);
    assert(hw_build_client_id("dev", 2, &tm, buf, sizeof(buf)) == E_INVAL);
    printf("  NULL/bad auth -> E_INVAL: PASS\n");
}

/* ─── ② password（HMAC-SHA256，独立向量）────────────────── */

static void test_password_rfc_vector(void)
{
    printf("--- test_password_rfc_vector (T03 ②, RFC4231-TC2) ---\n");

    /* HMAC-SHA256(key="Jefe", data="what do ya want for nothing?") */
    char hex[128];
    assert(hw_build_password("what do ya want for nothing?", "Jefe",
                             hex, sizeof(hex)) == E_OK);
    assert(strcmp(hex,
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0);
    assert(is_lower_hex64(hex));
    printf("  RFC4231-TC2 key/data order + hex: PASS\n");
}

static void test_password_oracle_vectors(void)
{
    printf("--- test_password_oracle_vectors (T03 ②, python oracle) ---\n");

    char hex[128];

    assert(hw_build_password("test-secret-123", "2023091516",
                             hex, sizeof(hex)) == E_OK);
    assert(strcmp(hex,
        "beb722273e8e8f115d606e568a3087fda0c31ca5c321e14e5d650bd3b40e9107") == 0);
    assert(is_lower_hex64(hex));

    assert(hw_build_password("test-secret-123", "2023091515",
                             hex, sizeof(hex)) == E_OK);
    assert(strcmp(hex,
        "6fc823c229b187edf5562eaa658b88213af03d024986c60504608e2d201b4611") == 0);

    assert(hw_build_password("MySecret", "2023030916",
                             hex, sizeof(hex)) == E_OK);
    assert(strcmp(hex,
        "cc4d31455943a1140a540fd48e464a8fbac1a7dc0aac65e39b8009e06886bae7") == 0);

    printf("  3 oracle vectors (64 lower hex): PASS\n");
}

static void test_password_defensive(void)
{
    printf("--- test_password_defensive (T03 ②) ---\n");

    char hex[128];
    assert(hw_build_password(NULL, "2023091516", hex, sizeof(hex)) == E_INVAL);
    assert(hw_build_password("s", NULL, hex, sizeof(hex)) == E_INVAL);
    assert(hw_build_password("s", "t", NULL, sizeof(hex)) == E_INVAL);
    assert(hw_build_password("s", "t", hex, 64) == E_INVAL);   /* 需 65 */
    assert(hw_build_password("s", "t", hex, 0) == E_INVAL);
    printf("  NULL/len<65 -> E_INVAL: PASS\n");
}

/* 联立：同一 ts 既进 client_id 后缀又作 HMAC key（ts 现算路径不串） */
static void test_password_clientid_joint(void)
{
    printf("--- test_password_clientid_joint (T03 ②) ---\n");

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 2023 - 1900; tm.tm_mon = 9 - 1;
    tm.tm_mday = 15; tm.tm_hour = 16;

    char cid[320];
    assert(hw_build_client_id("dev-001", 0, &tm, cid, sizeof(cid)) == E_OK);

    /* client_id 内嵌时间戳 */
    const char *ts_in_cid = strrchr(cid, '_') + 1;
    assert(strcmp(ts_in_cid, "2023091516") == 0);

    /* 同一 ts 作 HMAC key → 与 oracle A 一致 */
    char hex[128];
    assert(hw_build_password("test-secret-123", ts_in_cid,
                             hex, sizeof(hex)) == E_OK);
    assert(strcmp(hex,
        "beb722273e8e8f115d606e568a3087fda0c31ca5c321e14e5d650bd3b40e9107") == 0);
    printf("  same ts in client_id suffix & HMAC key: PASS\n");
}

/* ─── ③ keepalive 钳制 ──────────────────────────────────── */

static void test_clamp_keepalive(void)
{
    printf("--- test_clamp_keepalive (T03 ③) ---\n");

    int v;
    assert(hw_clamp_keepalive(29, &v) == E_OK && v == 30);
    assert(hw_clamp_keepalive(30, &v) == E_OK && v == 30);
    assert(hw_clamp_keepalive(100, &v) == E_OK && v == 100);
    assert(hw_clamp_keepalive(1200, &v) == E_OK && v == 1200);
    assert(hw_clamp_keepalive(1201, &v) == E_OK && v == 1200);
    assert(hw_clamp_keepalive(0, &v) == E_OK && v == 30);
    assert(hw_clamp_keepalive(-5, &v) == E_OK && v == 30);
    assert(hw_clamp_keepalive(60, NULL) == E_INVAL);
    printf("  29/30/100/1200/1201/0/-5 + NULL: PASS\n");
}

/* ─── ④ event_time ──────────────────────────────────────── */

static void test_event_time(void)
{
    printf("--- test_event_time (T03 ④) ---\n");

    char buf[32];
    assert(hw_format_event_time(0, buf, sizeof(buf)) == E_OK);
    assert(strcmp(buf, "19700101T000000Z") == 0);

    assert(hw_format_event_time(1694793600000LL, buf, sizeof(buf)) == E_OK);
    assert(strcmp(buf, "20230915T160000Z") == 0);

    /* 闰年 2/29 */
    assert(hw_format_event_time(1709210096000LL, buf, sizeof(buf)) == E_OK);
    assert(strcmp(buf, "20240229T123456Z") == 0);

    /* 月末 12/31 23:59:59 */
    assert(hw_format_event_time(1704067199000LL, buf, sizeof(buf)) == E_OK);
    assert(strcmp(buf, "20231231T235959Z") == 0);

    assert(hw_format_event_time(1000, NULL, sizeof(buf)) == E_INVAL);
    assert(hw_format_event_time(1000, buf, 16) == E_INVAL);   /* 需 17 */
    printf("  epoch/leap/month-end + len guard: PASS\n");
}

/* ─── ⑤ 主题拼装 / classify / extract_request_id ─────────── */

static void test_build_topic(void)
{
    printf("--- test_build_topic (T03 ⑤) ---\n");

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.huawei_device_id, sizeof(cfg.huawei_device_id), "gw-1");

    char buf[256];
    struct { enum hw_topic k; const char *want; } cases[] = {
        { HW_TOPIC_PROPERTIES_REPORT,
          "$oc/devices/gw-1/sys/properties/report" },
        { HW_TOPIC_BATCH,
          "$oc/devices/gw-1/sys/gateway/sub_devices/properties/report" },
        { HW_TOPIC_REGISTER,
          "$oc/devices/gw-1/sys/gateway/sub_devices/register" },
        { HW_TOPIC_REGISTER_RESPONSE,
          "$oc/devices/gw-1/sys/gateway/sub_devices/register/response" },
        { HW_TOPIC_STATUS,
          "$oc/devices/gw-1/sys/gateway/sub_devices/status" },
        { HW_TOPIC_COMMANDS_WILDCARD,
          "$oc/devices/gw-1/sys/commands/#" },
        { HW_TOPIC_EVENTS_REPORT,
          "$oc/devices/gw-1/sys/events/report" },
        { HW_TOPIC_COMMANDS_RESPONSE,
          "$oc/devices/gw-1/sys/commands/response" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        assert(hw_build_topic(&cfg, cases[i].k, buf, sizeof(buf)) == E_OK);
        assert(strcmp(buf, cases[i].want) == 0);
    }
    printf("  8 topic kinds exact: PASS\n");

    /* 防御 / 截断 */
    assert(hw_build_topic(NULL, HW_TOPIC_BATCH, buf, sizeof(buf)) == E_INVAL);
    assert(hw_build_topic(&cfg, HW_TOPIC_COUNT, buf, sizeof(buf)) == E_INVAL);
    cfg.huawei_device_id[0] = '\0';
    assert(hw_build_topic(&cfg, HW_TOPIC_BATCH, buf, sizeof(buf)) == E_INVAL);
    cfg.huawei_device_id[0] = 'g';
    assert(hw_build_topic(&cfg, HW_TOPIC_BATCH, buf, 8) == E_IO);
    printf("  bad args/count/empty-id/trunc -> E_INVAL/E_IO: PASS\n");
}

static void test_classify_topic(void)
{
    printf("--- test_classify_topic (T03 ⑤) ---\n");

    assert(hw_classify_topic("$oc/devices/gw-1/sys/commands/request_id=abc-123")
           == HW_KIND_CMD_REQUEST);
    assert(hw_classify_topic(
        "$oc/devices/gw-1/sys/gateway/sub_devices/register/response/request_id=x")
           == HW_KIND_REGISTER_RESP);

    /* 不误判 */
    assert(hw_classify_topic(
        "$oc/devices/gw-1/sys/commands/response/request_id=x") == HW_KIND_OTHER);
    assert(hw_classify_topic("$oc/devices/gw-1/sys/properties/report")
           == HW_KIND_OTHER);
    assert(hw_classify_topic("$oc/devices/gw-1/sys/commands/#")
           == HW_KIND_OTHER);
    assert(hw_classify_topic("embmqttnode/x/ota/cmd") == HW_KIND_OTHER);
    assert(hw_classify_topic(NULL) == HW_KIND_OTHER);
    printf("  CMD/REGISTER_RESP/OTHER (no misclass): PASS\n");
}

static void test_extract_request_id(void)
{
    printf("--- test_extract_request_id (T03 ⑤) ---\n");

    char buf[64];
    assert(hw_extract_request_id(
        "$oc/devices/gw-1/sys/commands/request_id=abc-123",
        buf, sizeof(buf)) == E_OK);
    assert(strcmp(buf, "abc-123") == 0);

    assert(hw_extract_request_id(
        "$oc/devices/gw-1/sys/gateway/sub_devices/register/response/request_id=x",
        buf, sizeof(buf)) == E_OK);
    assert(strcmp(buf, "x") == 0);

    /* 缺失 → E_NOT_FOUND */
    assert(hw_extract_request_id("$oc/devices/gw-1/sys/properties/report",
                                 buf, sizeof(buf)) == E_NOT_FOUND);
    /* 畸形（空值）→ E_INVAL */
    assert(hw_extract_request_id("$oc/devices/gw-1/sys/commands/request_id=",
                                 buf, sizeof(buf)) == E_INVAL);
    /* 放不下（拒绝而非截断）→ E_IO */
    assert(hw_extract_request_id(
        "$oc/devices/gw-1/sys/commands/request_id=0123456789",
        buf, 4) == E_IO);
    /* 防御 */
    assert(hw_extract_request_id(NULL, buf, sizeof(buf)) == E_INVAL);
    assert(hw_extract_request_id("t", NULL, sizeof(buf)) == E_INVAL);
    printf("  ok/missing/malformed/overflow + NULL: PASS\n");
}

/* ─── ⑥ payload builders ────────────────────────────────── */

static void make_entry(struct subdev_entry *e)
{
    memset(e, 0, sizeof(*e));
    e->src = SUBDEV_SENSOR;
    e->slave_id = 0;
    snprintf(e->sensor_type, sizeof(e->sensor_type), "mock");
    snprintf(e->device_id, sizeof(e->device_id), "sub-0001");
    snprintf(e->name, sizeof(e->name), "Mock-Sensor");
    snprintf(e->service_id, sizeof(e->service_id), "SensorData");
}

static void make_dev(struct device_info *dev)
{
    memset(dev, 0, sizeof(*dev));
    snprintf(dev->hostname, sizeof(dev->hostname), "e2ehost");
    snprintf(dev->mac_addr, sizeof(dev->mac_addr), "aa:bb:cc:dd:ee:ff");
    snprintf(dev->cpu_model, sizeof(dev->cpu_model), "e2ecpu");
    snprintf(dev->kernel_ver, sizeof(dev->kernel_ver), "6.6.0-e2e");
    dev->total_mem_kb = 1024;
}

static void test_build_gateway_props(void)
{
    printf("--- test_build_gateway_props (T03 ⑥) ---\n");

    struct node_config cfg; memset(&cfg, 0, sizeof(cfg));
    struct device_info dev; make_dev(&dev);

    char buf[512];
    assert(hw_build_gateway_props(&cfg, &dev, "online", buf, sizeof(buf)) == E_OK);
    assert(strstr(buf, "\"version\":\"" EMBMQTTNODE_VERSION "\"") != NULL);
    assert(strstr(buf, "\"status\":\"online\"") != NULL);
    assert(strstr(buf, "\"hostname\":\"e2ehost\"") != NULL);
    assert(strstr(buf, "\"mac\":\"aa:bb:cc:dd:ee:ff\"") != NULL);
    assert(strstr(buf, "\"mem_kb\":1024") != NULL);

    /* 截断 → E_IO；防御 */
    assert(hw_build_gateway_props(&cfg, &dev, "online", buf, 16) == E_IO);
    assert(hw_build_gateway_props(NULL, &dev, "online", buf, sizeof(buf)) == E_INVAL);
    assert(hw_build_gateway_props(&cfg, NULL, "online", buf, sizeof(buf)) == E_INVAL);
    assert(hw_build_gateway_props(&cfg, &dev, NULL, buf, sizeof(buf)) == E_INVAL);
    printf("  version/status/hostname/mac + trunc/defensive: PASS\n");
}

static void test_build_batch_report(void)
{
    printf("--- test_build_batch_report (T03 ⑥) ---\n");

    struct subdev_entry e; make_entry(&e);
    struct sensor_data d;
    memset(&d, 0, sizeof(d));
    d.temperature = 12.34; d.humidity = 56.78; d.pressure = 1013.25;
    d.timestamp_ms = 1694793600000LL;   /* 2023-09-15T16:00:00Z */

    char buf[512];
    assert(hw_build_batch_report(&e, &d, buf, sizeof(buf)) == E_OK);
    assert(strstr(buf, "\"device_id\":\"sub-0001\"") != NULL);
    assert(strstr(buf, "\"service_id\":\"SensorData\"") != NULL);
    assert(strstr(buf, "\"temperature\":12.34") != NULL);
    assert(strstr(buf, "\"humidity\":56.79") != NULL || strstr(buf, "\"humidity\":56.78") != NULL);
    assert(strstr(buf, "\"pressure\":1013.25") != NULL);
    assert(strstr(buf, "\"event_time\":\"20230915T160000Z\"") != NULL);
    printf("  all-valid batch + event_time: PASS\n");

    /* mask：pressure 无效 → 整体省略 */
    d.pressure = SENSOR_VALUE_INVALID;
    assert(hw_build_batch_report(&e, &d, buf, sizeof(buf)) == E_OK);
    assert(strstr(buf, "pressure") == NULL);
    assert(strstr(buf, "\"temperature\":12.34") != NULL);
    printf("  sentinel field omitted (mask): PASS\n");

    /* 三字段全空 → E_NOT_FOUND */
    d.temperature = SENSOR_VALUE_INVALID;
    d.humidity = SENSOR_VALUE_INVALID;
    assert(hw_build_batch_report(&e, &d, buf, sizeof(buf)) == E_NOT_FOUND);
    printf("  all-invalid -> E_NOT_FOUND: PASS\n");

    /* 截断 → E_IO；防御 */
    d.temperature = 1.0; d.humidity = 2.0; d.pressure = 3.0;
    assert(hw_build_batch_report(&e, &d, buf, 16) == E_IO);
    assert(hw_build_batch_report(NULL, &d, buf, sizeof(buf)) == E_INVAL);
    assert(hw_build_batch_report(&e, NULL, buf, sizeof(buf)) == E_INVAL);
    printf("  trunc -> E_IO + NULL defensive: PASS\n");
}

static void test_build_register_status(void)
{
    printf("--- test_build_register_status (T03 ⑥) ---\n");

    struct subdev_entry e; make_entry(&e);
    char buf[256];

    assert(hw_build_subdev_register(&e, buf, sizeof(buf)) == E_OK);
    assert(strstr(buf, "\"device_id\":\"sub-0001\"") != NULL);
    assert(strstr(buf, "\"name\":\"Mock-Sensor\"") != NULL);

    assert(hw_build_subdev_status(&e, 1, buf, sizeof(buf)) == E_OK);
    assert(strstr(buf, "\"status\":\"ONLINE\"") != NULL);
    assert(hw_build_subdev_status(&e, 0, buf, sizeof(buf)) == E_OK);
    assert(strstr(buf, "\"status\":\"OFFLINE\"") != NULL);

    assert(hw_build_subdev_register(&e, buf, 8) == E_IO);
    assert(hw_build_subdev_register(NULL, buf, sizeof(buf)) == E_INVAL);
    assert(hw_build_subdev_status(NULL, 1, buf, sizeof(buf)) == E_INVAL);
    printf("  register + ONLINE/OFFLINE + guards: PASS\n");
}

static void test_build_event_alert(void)
{
    printf("--- test_build_event_alert (T03 ⑥) ---\n");

    struct alert_event evt;
    memset(&evt, 0, sizeof(evt));
    snprintf(evt.rule_name, sizeof(evt.rule_name), "high-temp");
    snprintf(evt.field, sizeof(evt.field), "temperature");
    evt.value = 88.50;
    evt.threshold = 80.00;
    snprintf(evt.source_kind, sizeof(evt.source_kind), "modbus");
    evt.source_id = 3;
    snprintf(evt.msg, sizeof(evt.msg), "temperature above threshold");
    evt.ts_ms = 1694793600000LL;

    char buf[768];
    assert(hw_build_event_alert(&evt, "sub-0001", buf, sizeof(buf)) == E_OK);
    assert(strstr(buf, "\"event_type\":\"alert\"") != NULL);
    assert(strstr(buf, "\"event_time\":\"20230915T160000Z\"") != NULL);
    assert(strstr(buf, "\"rule_name\":\"high-temp\"") != NULL);
    assert(strstr(buf, "\"field\":\"temperature\"") != NULL);
    assert(strstr(buf, "\"value\":88.50") != NULL);
    assert(strstr(buf, "\"threshold\":80.00") != NULL);
    assert(strstr(buf, "\"source\":\"modbus\"") != NULL);
    assert(strstr(buf, "\"source_id\":\"sub-0001\"") != NULL);
    assert(strstr(buf, "\"msg\":\"temperature above threshold\"") != NULL);

    assert(hw_build_event_alert(&evt, "s", buf, 16) == E_IO);
    assert(hw_build_event_alert(NULL, "s", buf, sizeof(buf)) == E_INVAL);
    printf("  alert event fields + event_time + guards: PASS\n");
}

static void test_build_command_response(void)
{
    printf("--- test_build_command_response (T03 ⑥) ---\n");

    char buf[256];
    assert(hw_build_command_response("abc-123", 0, "upgrade accepted",
                                     buf, sizeof(buf)) == E_OK);
    assert(strstr(buf, "\"result_code\":0") != NULL);
    assert(strstr(buf, "\"result_msg\":\"upgrade accepted\"") != NULL);
    assert(strstr(buf, "\"request_id\":\"abc-123\"") != NULL);

    assert(hw_build_command_response("x", 2, "unknown command",
                                     buf, sizeof(buf)) == E_OK);
    assert(strstr(buf, "\"result_code\":2") != NULL);

    assert(hw_build_command_response("x", 0, NULL, buf, sizeof(buf)) == E_INVAL);
    assert(hw_build_command_response("x", 0, "m", buf, 8) == E_IO);
    printf("  code/msg/request_id + guards: PASS\n");
}

/* ─── ⑦ connect_params 装配 ─────────────────────────────── */

static void test_connect_params(void)
{
    printf("--- test_connect_params (T03 ⑦) ---\n");

    struct node_config cfg;
    struct platform_connect_params p;

    /* 空凭据 → E_INVAL（供 T04 select 回落判定） */
    memset(&cfg, 0, sizeof(cfg));
    assert(hw_connect_params(&cfg, &p) == E_INVAL);
    snprintf(cfg.huawei_device_id, sizeof(cfg.huawei_device_id), "gw-1");
    assert(hw_connect_params(&cfg, &p) == E_INVAL);   /* 缺 secret */
    assert(hw_connect_params(NULL, &p) == E_INVAL);
    assert(hw_connect_params(&cfg, NULL) == E_INVAL);
    printf("  empty id/secret -> E_INVAL: PASS\n");

    /* auth_type=0 */
    snprintf(cfg.huawei_secret, sizeof(cfg.huawei_secret), "test-secret-123");
    cfg.huawei_auth_type = 0;
    cfg.huawei_keepalive = 120;
    snprintf(cfg.huawei_ca_file, sizeof(cfg.huawei_ca_file), "/etc/hw/ca.pem");
    assert(hw_connect_params(&cfg, &p) == E_OK);
    assert(strcmp(p.username, "gw-1") == 0);
    assert(p.force_tls == 1);
    assert(p.use_will == 0);
    assert(p.keepalive == 120);
    assert(p.rebuild_on_hour == 0);
    assert(strcmp(p.ca_file, "/etc/hw/ca.pem") == 0);
    assert(strncmp(p.client_id, "gw-1_0_0_", 9) == 0);
    assert(is_lower_hex64(p.password));
    assert(p.built_ts > 0);
    printf("  auth0: user/force_tls/use_will/ca/64hex: PASS\n");

    /* auth_type=1 → rebuild_on_hour=1 + client_id 含 _0_1_ */
    cfg.huawei_auth_type = 1;
    assert(hw_connect_params(&cfg, &p) == E_OK);
    assert(p.rebuild_on_hour == 1);
    assert(strncmp(p.client_id, "gw-1_0_1_", 9) == 0);
    printf("  auth1: rebuild_on_hour=1: PASS\n");

    /* keepalive 钳制经装配生效 */
    cfg.huawei_keepalive = 5000;
    assert(hw_connect_params(&cfg, &p) == E_OK);
    assert(p.keepalive == 1200);
    printf("  keepalive clamp via assembly: PASS\n");
}

/* ─── 入口 ─────────────────────────────────────────────── */

int main(void)
{
    printf("=== Platform Huawei Pure-Function Tests (T03) ===\n\n");

    test_client_id_format();
    test_client_id_gmtime();
    test_client_id_boundaries();

    test_password_rfc_vector();
    test_password_oracle_vectors();
    test_password_defensive();
    test_password_clientid_joint();

    test_clamp_keepalive();
    test_event_time();

    test_build_topic();
    test_classify_topic();
    test_extract_request_id();

    test_build_gateway_props();
    test_build_batch_report();
    test_build_register_status();
    test_build_event_alert();
    test_build_command_response();

    test_connect_params();

    printf("\n=== ALL platform_huawei tests PASSED ===\n");
    return 0;
}
