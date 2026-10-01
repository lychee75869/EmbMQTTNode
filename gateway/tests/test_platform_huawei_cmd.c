/*
 * tests/test_platform_huawei_cmd.c
 * huawei 命令闭环单测——第 14 个测试（无 broker）。
 *
 * 覆盖：
 *   ① hw_cmd_parse 完整性矩阵（缺字段/空串/非字符串/未知命令）
 *   ② rid 缺失/空值 → 不回执（丢弃）
 *   ③ 回执码 + 回执主题（base + /request_id=rid）拼接
 *   ④ 回执主题拼接截断 → 拒绝发送
 *   ⑤ OTA 桥接：受理（code=0）/ busy（code=1）；ota.c 零改动（走 ota_state_string）
 *   ⑥ OTA 桥接：pubkey 未配置 → 拒绝（code=1）
 *   ⑦ reboot：先回执后停机（注入 cb 断言顺序）
 *   ⑧ hw_build_ota_status_event schema
 *   ⑨ hw_ota_status_shim 映射（注入 publish 桩；不递归）+ platform_ota_status_publish 路由
 *
 * 无 broker：经 hw_cmd_set_publisher() 注入发布桩（默认后端 mqtt_publish_raw）；
 * 生产路径不受影响。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "../src/common.h"
#include "../src/platform.h"
#include "../src/platform_huawei.h"
#include "../src/mqtt_client.h"
#include "../src/ota.h"

/* ─── 发布桩 ───────────────────────────────────────────────── */

static int  g_pub_calls = 0;
static int  g_pub_rc = E_OK;
static char g_topic[512];
static char g_payload[1024];

static int stub_pub(const char *topic, const char *payload, int qos)
{
    (void)qos;
    g_pub_calls++;
    snprintf(g_topic, sizeof(g_topic), "%s", topic ? topic : "");
    snprintf(g_payload, sizeof(g_payload), "%s", payload ? payload : "");
    return g_pub_rc;
}

static void reset_stub(void)
{
    g_pub_calls = 0;
    g_pub_rc = E_OK;
    g_topic[0] = '\0';
    g_payload[0] = '\0';
}

/* 从回执 payload 取 result_code（未找到 → -99） */
static int resp_code(void)
{
    const char *p = strstr(g_payload, "\"result_code\":");
    if (!p)
        return -99;
    return atoi(p + strlen("\"result_code\":"));
}

/* ─── ota_handle_message 捕获桩（-Wl,--wrap；QA 原样用例回归）─────
 * 记录调用次数与「重建后交给 ota.c 的真实入参」，用于断言命令 JSON
 * 注入被 fail-closed 拒绝后 ota_handle_message 调用次数为 0。 */
extern int __real_ota_handle_message(const char *payload, int len);

static int  g_ota_calls = 0;
static char g_ota_json[2048];

int __wrap_ota_handle_message(const char *payload, int len)
{
    g_ota_calls++;
    int n = (len < (int)sizeof(g_ota_json) - 1) ? len : (int)sizeof(g_ota_json) - 1;
    if (n < 0)
        n = 0;
    memcpy(g_ota_json, payload, (size_t)n);
    g_ota_json[n] = '\0';
    return __real_ota_handle_message(payload, len);
}

static int count_substr(const char *hay, const char *needle)
{
    int c = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += strlen(needle);
    }
    return c;
}

/* ─── 平台装配 ─────────────────────────────────────────────── */

static char g_conf[256];
static struct node_config g_cfg;

static void setup_platform(void)
{
    snprintf(g_conf, sizeof(g_conf), "/tmp/subdev_%d.conf", (int)getpid());
    FILE *f = fopen(g_conf, "w");
    if (f) { fputs("# empty registry\n", f); fclose(f); }

    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.platform, sizeof(g_cfg.platform), "huawei");
    snprintf(g_cfg.huawei_device_id, sizeof(g_cfg.huawei_device_id), "gw-123");
    snprintf(g_cfg.huawei_secret, sizeof(g_cfg.huawei_secret), "secret-x");
    snprintf(g_cfg.subdevices_conf, sizeof(g_cfg.subdevices_conf), "%s", g_conf);

    assert(platform_select(&g_cfg) == E_OK);   /* 置 platform_node_config + huawei 激活 */
    hw_cmd_set_publisher(stub_pub);
}

static void cmd_topic(char *buf, int n, const char *rid)
{
    snprintf(buf, (size_t)n, "$oc/devices/gw-123/sys/commands/request_id=%s", rid);
}

/* 便捷：驱动一条命令并返回 result_code */
static int drive(const char *rid, const char *payload)
{
    char topic[256];
    cmd_topic(topic, (int)sizeof(topic), rid);
    reset_stub();
    hw_cmd_handle(topic, payload, (int)strlen(payload));
    return resp_code();
}

/* ─── ① 解析完整性矩阵（ota 未启用）────────────────────────── */

static void test_parse_matrix(void)
{
    printf("--- test_parse_matrix ---\n");

    /* 未知命令 / 缺 command_name / 非字符串 command_name → code=2 */
    assert(drive("r1", "{\"command_name\":\"set_temp\"}") == 2);
    assert(drive("r2", "{\"paras\":{}}") == 2);
    assert(drive("r3", "{\"command_name\":123,\"paras\":{}}") == 2);
    assert(drive("r4", "{\"command_name\":\"\"}") == 2);
    printf("  unknown/missing/non-string command_name -> code=2: PASS\n");

    /* ota_upgrade：cmd 缺失 / 非 upgrade → code=1 */
    assert(drive("r5",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"version\":\"1.0\",\"url\":\"http://x\"}}") == 1);
    assert(drive("r6",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"cmd\":\"download\",\"version\":\"1.0\",\"url\":\"http://x\"}}") == 1);
    printf("  ota cmd missing/!=upgrade -> code=1: PASS\n");

    /* version 缺失 / 空串；url 缺失 / 空串 → code=1 */
    assert(drive("r7",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"cmd\":\"upgrade\",\"url\":\"http://x\"}}") == 1);
    assert(drive("r8",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"cmd\":\"upgrade\",\"version\":\"\",\"url\":\"http://x\"}}") == 1);
    assert(drive("r9",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"cmd\":\"upgrade\",\"version\":\"1.0\"}}") == 1);
    assert(drive("r10",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"cmd\":\"upgrade\",\"version\":\"1.0\",\"url\":\"\"}}") == 1);
    printf("  version/url missing or empty -> code=1: PASS\n");

    /* checksum 非字符串 → code=1 */
    assert(drive("r11",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"cmd\":\"upgrade\",\"version\":\"1.0\",\"url\":\"http://x\","
        "\"checksum\":123}}") == 1);
    printf("  checksum non-string -> code=1: PASS\n");

    /* 合法 ota 但 ota 未启用 → 桥接后仍 idle → code=1（rejected） */
    assert(drive("r12",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"cmd\":\"upgrade\",\"version\":\"1.0\",\"url\":\"http://x\","
        "\"checksum\":\"deadbeef\"}}") == 1);
    printf("  ota disabled -> upgrade rejected code=1: PASS\n");
}

/* ─── ② rid 缺失/空值 → 不回执 ─────────────────────────────── */

static void test_rid_drop(void)
{
    printf("--- test_rid_drop ---\n");

    reset_stub();
    hw_cmd_handle("$oc/devices/gw-123/sys/commands/foo",
                  "{\"command_name\":\"reboot\"}", 25);
    assert(g_pub_calls == 0);
    printf("  no request_id -> drop (no reply): PASS\n");

    reset_stub();
    hw_cmd_handle("$oc/devices/gw-123/sys/commands/request_id=",
                  "{\"command_name\":\"reboot\"}", 25);
    assert(g_pub_calls == 0);
    printf("  empty request_id -> drop (no reply): PASS\n");
}

/* ─── ③ 回执码 + 主题拼接 ──────────────────────────────────── */

static void test_response_topic(void)
{
    printf("--- test_response_topic ---\n");

    char topic[256];
    cmd_topic(topic, (int)sizeof(topic), "r-abc");
    reset_stub();
    hw_cmd_handle(topic, "{\"command_name\":\"reboot\"}", 24);

    assert(g_pub_calls == 1);
    assert(strcmp(g_topic,
        "$oc/devices/gw-123/sys/commands/response/request_id=r-abc") == 0);
    assert(resp_code() == 0);
    assert(strstr(g_payload, "rebooting") != NULL);
    printf("  reply topic=base+rid, code=0: PASS\n");
}

/* ─── ④ 主题拼接截断 → 拒绝 ────────────────────────────────── */

static void test_resp_topic_truncation(void)
{
    printf("--- test_resp_topic_truncation ---\n");

    /* device_id 220 + rid 60：base 255 fits(256)，base + "/request_id=" + rid
     * = 327 > 320 → 拒绝而非截断 */
    static struct node_config big;
    char devid[221], rid[61], topic[512];
    memset(devid, 'D', 220); devid[220] = '\0';
    memset(rid, 'R', 60);    rid[60] = '\0';
    snprintf(topic, sizeof(topic),
             "$oc/devices/%s/sys/commands/request_id=%s", devid, rid);

    memset(&big, 0, sizeof(big));
    snprintf(big.platform, sizeof(big.platform), "huawei");
    snprintf(big.huawei_device_id, sizeof(big.huawei_device_id), "%s", devid);
    snprintf(big.huawei_secret, sizeof(big.huawei_secret), "secret-x");
    snprintf(big.subdevices_conf, sizeof(big.subdevices_conf), "%s", g_conf);
    assert(platform_select(&big) == E_OK);

    reset_stub();
    hw_cmd_handle(topic, "{\"command_name\":\"reboot\"}", 24);
    assert(g_pub_calls == 0);
    printf("  oversized reply topic -> refuse (no publish): PASS\n");

    /* 还原全局 cfg */
    assert(platform_select(&g_cfg) == E_OK);
}

/* ─── ⑤ OTA 桥接：受理 / busy ──────────────────────────────── */

static void test_ota_bridge(void)
{
    printf("--- test_ota_bridge ---\n");

    static char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/ota_%d", (int)getpid());

    struct ota_config ocfg;
    memset(&ocfg, 0, sizeof(ocfg));
    ocfg.enabled = 1;
    ocfg.boot_attempt_max = 3;
    snprintf(ocfg.slot_dir, sizeof(ocfg.slot_dir), "%s", dir);
    /* public_key 非空即可（本单测不跑验签线程）；经中转缓冲避免
     * -Wformat-truncation（dir 可达 255B） */
    {
        char tmp[512];
        int n = snprintf(tmp, sizeof(tmp), "%s/dummy_pub.pem", dir);
        assert(n > 0 && (size_t)n < sizeof(ocfg.public_key));
        memcpy(ocfg.public_key, tmp, (size_t)n + 1);
    }

    assert(ota_init(&ocfg, "gw-123", "1.0") == E_OK);
    assert(strcmp(ota_state_string(), "idle") == 0);

    const char *valid =
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"cmd\":\"upgrade\",\"version\":\"2.0.1\",\"url\":\"https://x/fw\","
        "\"checksum\":\"sha256:abc\"}}";

    /* 首次受理 → code=0 "upgrade accepted"（后置复核 != idle） */
    int c1 = drive("o1", valid);
    assert(c1 == 0);
    assert(strstr(g_payload, "upgrade accepted") != NULL);
    assert(strcmp(ota_state_string(), "idle") != 0);   /* 已进 DOWNLOADING */
    printf("  ota accept -> code=0 accepted: PASS\n");

    /* 第二次 → 前置 busy → code=1 "device busy" */
    int c2 = drive("o2", valid);
    assert(c2 == 1);
    assert(strstr(g_payload, "device busy") != NULL);
    printf("  ota busy -> code=1 device busy: PASS\n");

    ota_close();
    assert(strcmp(ota_state_string(), "idle") == 0);
}

/* ─── ⑥ OTA 桥接：pubkey 未配置 → 拒绝 ─────────────────────── */

static void test_ota_reject_pubkey(void)
{
    printf("--- test_ota_reject_pubkey ---\n");

    static char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/ota2_%d", (int)getpid());

    struct ota_config ocfg;
    memset(&ocfg, 0, sizeof(ocfg));
    ocfg.enabled = 1;
    snprintf(ocfg.slot_dir, sizeof(ocfg.slot_dir), "%s", dir);
    /* public_key 留空 → ota_handle_message fail-closed 拒绝，状态保持 idle */

    assert(ota_init(&ocfg, "gw-123", "1.0") == E_OK);
    assert(strcmp(ota_state_string(), "idle") == 0);

    const char *valid =
        "{\"command_name\":\"ota_upgrade\",\"paras\":{"
        "\"cmd\":\"upgrade\",\"version\":\"2.0.1\",\"url\":\"https://x/fw\"}}";

    int c = drive("p1", valid);   /* 无 checksum：本桥接重建为 "checksum":"" */
    assert(c == 1);
    assert(strstr(g_payload, "upgrade rejected") != NULL);
    assert(strcmp(ota_state_string(), "idle") == 0);   /* 仍 idle = 被拒 */
    printf("  pubkey missing -> code=1 rejected: PASS\n");

    ota_close();
}

/* ─── ⑦ reboot：先回执后停机 ───────────────────────────────── */

static int g_reboot_called = 0;
static int g_reboot_pubcalls_at_call = -1;

static void reboot_cb(void)
{
    g_reboot_called++;
    g_reboot_pubcalls_at_call = g_pub_calls;   /* 记录停机时已发出的回执数 */
}

static void test_reboot_order(void)
{
    printf("--- test_reboot_order ---\n");

    g_reboot_called = 0;
    g_reboot_pubcalls_at_call = -1;
    platform_set_reboot_request(reboot_cb);

    char topic[256];
    cmd_topic(topic, (int)sizeof(topic), "rb1");
    reset_stub();
    hw_cmd_handle(topic, "{\"command_name\":\"reboot\"}", 24);

    /* 顺序断言：停机回调被调用时，回执已发出（g_pub_calls>=1） */
    assert(g_reboot_called == 1);
    assert(g_reboot_pubcalls_at_call >= 1);
    assert(resp_code() == 0);
    assert(strstr(g_payload, "rebooting") != NULL);
    printf("  reply sent BEFORE reboot callback: PASS\n");

    platform_set_reboot_request(NULL);
}

/* ─── ⑧ hw_build_ota_status_event ──────────────────────────── */

static void test_build_event(void)
{
    printf("--- test_build_ota_status_event ---\n");

    char buf[512];
    int rc = hw_build_ota_status_event("downloading", "2.0.1",
                                       1694793600000LL, buf, (int)sizeof(buf));
    assert(rc == E_OK);
    assert(strstr(buf, "\"service_id\":\"Gateway\"") != NULL);
    assert(strstr(buf, "\"event_type\":\"ota_status\"") != NULL);
    assert(strstr(buf, "\"state\":\"downloading\"") != NULL);
    assert(strstr(buf, "\"version\":\"2.0.1\"") != NULL);
    {
        const char *p = strstr(buf, "\"event_time\":\"");
        assert(p != NULL);
        p += strlen("\"event_time\":\"");
        assert(strlen(p) >= 17);
        assert(p[8] == 'T' && p[15] == 'Z');   /* yyyyMMddThhmmssZ */
    }
    printf("  schema + event_time format: PASS\n");

    /* 防御：NULL state / 小缓冲 */
    assert(hw_build_ota_status_event(NULL, "v", 0, buf, (int)sizeof(buf)) == E_INVAL);
    char tiny[8];
    assert(hw_build_ota_status_event("running", "v", 0, tiny,
                                     (int)sizeof(tiny)) == E_IO);

    /* state / version 含 JSON 逸出字符 → fail-closed 拒绝 */
    assert(hw_build_ota_status_event("down\"loading", "v", 0,
                                     buf, (int)sizeof(buf)) == E_INVAL);
    assert(hw_build_ota_status_event("running", "2.0\\x", 0,
                                     buf, (int)sizeof(buf)) == E_INVAL);
    assert(hw_build_ota_status_event("a\nb", "v", 0,
                                     buf, (int)sizeof(buf)) == E_INVAL);
    printf("  NULL/small buffer + unsafe chars -> E_INVAL/E_IO: PASS\n");
}

/* ─── ⑨ shim 映射 + 平台路由 ───────────────────────────────── */

static void test_shim(void)
{
    printf("--- test_ota_status_shim ---\n");

    reset_stub();
    int rc = hw_ota_status_shim("embmqttnode/gw-123/ota/status",
                                "{\"state\":\"installing\",\"version\":\"2.0\"}", 1);
    assert(rc == E_OK);
    assert(g_pub_calls == 1);
    assert(strstr(g_topic, "/sys/events/report") != NULL);
    assert(strstr(g_payload, "\"event_type\":\"ota_status\"") != NULL);
    assert(strstr(g_payload, "\"state\":\"installing\"") != NULL);
    assert(strstr(g_payload, "\"version\":\"2.0\"") != NULL);
    printf("  shim -> events/report ota_status: PASS\n");

    /* 无 state → E_INVAL 且不发 */
    reset_stub();
    assert(hw_ota_status_shim("t", "{\"version\":\"2.0\"}", 1) == E_INVAL);
    assert(g_pub_calls == 0);
    printf("  missing state -> E_INVAL, no publish: PASS\n");

    /* state 含 " → 事件构建 E_INVAL 且不发布（由 shim 侧拒绝） */
    reset_stub();
    assert(hw_ota_status_shim("t", "{\"state\":\"bad\\\"state\"}", 1) == E_INVAL);
    assert(g_pub_calls == 0);
    printf("  unsafe state -> E_INVAL, no publish: PASS\n");

    /* platform_ota_status_publish：huawei 激活 → 走 shim */
    reset_stub();
    assert(platform_ota_status_publish("local/topic",
        "{\"state\":\"rebooting\",\"version\":\"2.0\"}", 1) == E_OK);
    assert(strstr(g_topic, "/sys/events/report") != NULL);
    printf("  platform_ota_status_publish(huawei) -> shim: PASS\n");

    /* local 激活 → 转发 mqtt_publish_raw（无 broker → E_INVAL，无 shim 事件） */
    static struct node_config lcfg;
    memset(&lcfg, 0, sizeof(lcfg));
    snprintf(lcfg.platform, sizeof(lcfg.platform), "local");
    assert(platform_select(&lcfg) == E_OK);
    reset_stub();
    assert(platform_ota_status_publish("local/topic",
        "{\"state\":\"running\"}", 1) == E_INVAL);
    assert(g_pub_calls == 0);
    printf("  platform_ota_status_publish(local) -> raw (E_INVAL no broker): PASS\n");

    assert(platform_select(&g_cfg) == E_OK);   /* 还原 huawei */
}

/* ─── ⑩ 命令 JSON 注入 fail-closed 拒绝（QA 原样用例）───── */

static void test_injection_blocked(void)
{
    printf("--- test_injection_blocked ---\n");

    static char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/ota_inj_%d", (int)getpid());

    struct ota_config ocfg;
    memset(&ocfg, 0, sizeof(ocfg));
    ocfg.enabled = 1;
    snprintf(ocfg.slot_dir, sizeof(ocfg.slot_dir), "%s", dir);
    {
        char tmp[512];
        int n = snprintf(tmp, sizeof(tmp), "%s/pub.pem", dir);
        assert(n > 0 && (size_t)n < sizeof(ocfg.public_key));
        memcpy(ocfg.public_key, tmp, (size_t)n + 1);
    }
    assert(ota_init(&ocfg, "gw-123", "1.0") == E_OK);

    /* QA 原样注入：url 值内用 \" 逸出，夹带伪造 checksum 键。
     * 修复前：重建 JSON 出现两个 "checksum":，伪造值覆盖真实值且 code=0。 */
    const char *attack =
        "{\"command_name\":\"ota_upgrade\",\"paras\":{\"cmd\":\"upgrade\","
        "\"version\":\"1.0\","
        "\"url\":\"https://fw.example/fw.bin\\\",\\\"checksum\\\":\\\"sha256:ATTACKER\","
        "\"checksum\":\"sha256:REAL\"}}";

    g_ota_calls = 0;
    g_ota_json[0] = '\0';
    int c = drive("inj1", attack);
    assert(c == 1);                               /* 被拒绝 */
    assert(g_ota_calls == 0);                     /* 未进入桥接 */
    assert(count_substr(g_ota_json, "\"checksum\":") < 2);   /* 无双重 checksum */
    printf("  QA injection -> code=1, ota_calls=0: PASS\n");

    /* version/url/checksum 分别含 " \ \n \t → 全部 code=1、不桥接 */
    const char *cases[] = {
        "{\"command_name\":\"ota_upgrade\",\"paras\":{\"cmd\":\"upgrade\","
        "\"version\":\"1.0\\\"x\",\"url\":\"https://x\",\"checksum\":\"c\"}}",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{\"cmd\":\"upgrade\","
        "\"version\":\"1.0\",\"url\":\"https://x\\\\y\",\"checksum\":\"c\"}}",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{\"cmd\":\"upgrade\","
        "\"version\":\"1.0\",\"url\":\"https://x\",\"checksum\":\"a\\nb\"}}",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{\"cmd\":\"upgrade\","
        "\"version\":\"1.0\",\"url\":\"https://x\",\"checksum\":\"a\\tb\"}}",
        "{\"command_name\":\"ota_upgrade\",\"paras\":{\"cmd\":\"upgrade\","
        "\"version\":\"1.0\",\"url\":\"a\\\"b\",\"checksum\":\"c\"}}",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_ota_calls = 0;
        char rid[16];
        snprintf(rid, sizeof(rid), "case%zu", i);
        int rc = drive(rid, cases[i]);
        assert(rc == 1);
        assert(g_ota_calls == 0);
    }
    printf("  version/url/checksum with quote/backslash/nl/tab -> code=1, no bridge: PASS\n");

    /* 合法对照（防过度拒绝）：正常 URL/version/sha256 → code=0 受理 */
    const char *ok =
        "{\"command_name\":\"ota_upgrade\",\"paras\":{\"cmd\":\"upgrade\","
        "\"version\":\"2.0.1\",\"url\":\"https://fw.example/fw.bin\","
        "\"checksum\":\"sha256:deadbeef\"}}";
    g_ota_calls = 0;
    g_ota_json[0] = '\0';
    int cok = drive("ok1", ok);
    assert(cok == 0);
    assert(g_ota_calls == 1);
    assert(count_substr(g_ota_json, "\"checksum\":") == 1);
    assert(strstr(g_ota_json, "sha256:deadbeef") != NULL);
    printf("  valid control -> code=0, single checksum key: PASS\n");

    ota_close();
}

/* ─── ⑪ hw_json_str_safe 边界 ─────────────────────────── */

static void test_json_str_safe(void)
{
    printf("--- test_json_str_safe ---\n");

    assert(hw_json_str_safe(NULL) == 0);
    assert(hw_json_str_safe("") == 1);
    assert(hw_json_str_safe("abc 123 ~") == 1);
    assert(hw_json_str_safe("a\"b") == 0);
    assert(hw_json_str_safe("a\\b") == 0);
    assert(hw_json_str_safe("a\nb") == 0);
    assert(hw_json_str_safe("a\tb") == 0);
    assert(hw_json_str_safe("\x01") == 0);
    assert(hw_json_str_safe("\x1f") == 0);
    assert(hw_json_str_safe(" ") == 1);              /* 空格 0x20 安全 */
    assert(hw_json_str_safe("\xe4\xb8\xad") == 1);   /* 中文 UTF-8 ≥0x80 安全 */
    printf("  boundary: NULL/\" \\ nl/tab/ctrl unsafe; space/UTF-8 safe: PASS\n");
}

/* ─── 入口 ─────────────────────────────────────────────────── */

int main(void)
{
    printf("=== Platform Huawei CMD Tests ===\n\n");

    setup_platform();

    test_parse_matrix();
    test_rid_drop();
    test_response_topic();
    test_resp_topic_truncation();
    test_ota_bridge();
    test_ota_reject_pubkey();
    test_reboot_order();
    test_build_event();
    test_shim();
    test_injection_blocked();
    test_json_str_safe();

    hw_cmd_set_publisher(NULL);
    unlink(g_conf);

    printf("\n=== ALL platform_huawei_cmd tests PASSED ===\n");
    return 0;
}
