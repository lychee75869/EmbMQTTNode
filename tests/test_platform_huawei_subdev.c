/*
 * tests/test_platform_huawei_subdev.c
 * T04 huawei 子设备管理单测——第 13 个测试（docs/13 Q2/Q3/Q4；无 broker）。
 *
 * 覆盖：
 *   ① subdev_count()/subdev_at() 边界
 *   ② hw_subdev_init 空文件/合法文件 → g_subdev_rt_count 与 rt[i].e 对齐
 *   ③ publish_data 返回码契约（命中→E_OK、未命中→E_OK、全空→E_OK、截断→E_IO）
 *   ④ publish_status / publish_alert topic 与 payload 路由
 *   ⑤ tick 退避单调性（纯函数 hw_subdev_backoff_ms + next_reg_ms 门控）
 *   ⑥ platform_select 装配（凭据齐→huawei；凭据缺→回落 local）
 *   ⑦ on_message REGISTER_RESP → registered 置位
 *
 * 说明：docs/13 验收 #1 明确「无 broker」。为使「命中→E_OK」等返回码可在
 * 无 broker 下单测，本测试经 hw_subdev_set_publisher() 注入发布桩（默认
 * 后端为 mqtt_publish_raw）；生产路径不受影响。
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include "../src/common.h"
#include "../src/platform.h"
#include "../src/platform_huawei.h"
#include "../src/subdev_registry.h"

/* ─── 临时注册表文件 ───────────────────────────────────────── */

static char g_path[256];

static void write_conf(const char *content)
{
    if (g_path[0] == '\0')
        snprintf(g_path, sizeof(g_path), "/tmp/t04_subdev_%d.conf", (int)getpid());
    FILE *f = fopen(g_path, "w");
    assert(f != NULL);
    fputs(content, f);
    fclose(f);
}

/* v1.4.0 网关纯化：SUBDEV_SENSOR 通道移除 → 注册表一律 modbus 条目 */
#define CONF_2 \
    "subdevice_1 = modbus,1,dev-mb-1,MB1\n" \
    "subdevice_2 = modbus,3,dev-mb-3,MB3\n"

/* ─── 发布桩 ───────────────────────────────────────────────── */

static int  g_pub_rc = E_OK;
static int  g_pub_calls = 0;
static char g_last_topic[256];
static char g_last_payload[1024];

static int stub_pub(const char *topic, const char *payload, int qos)
{
    (void)qos;
    g_pub_calls++;
    snprintf(g_last_topic, sizeof(g_last_topic), "%s", topic ? topic : "");
    snprintf(g_last_payload, sizeof(g_last_payload), "%s", payload ? payload : "");
    return g_pub_rc;
}

static void make_cfg(struct node_config *cfg, const char *conf_path)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->subdevices_conf, sizeof(cfg->subdevices_conf), "%s", conf_path);
    /* 合法 device_id → hw_build_topic 成功（否则 publish_data 会因建主题失败
     * 提前返回 E_IO，无法验证返回码契约）。凭据非法场景由用例显式清空。 */
    snprintf(cfg->huawei_device_id, sizeof(cfg->huawei_device_id), "gw-123");
    snprintf(cfg->huawei_secret, sizeof(cfg->huawei_secret), "secret-x");
    cfg->huawei_props_interval = 3600;   /* 避免 tick ① 干扰退避观测 */
    cfg->subdev_offline_sec    = 30;
}

/* ─── ① subdev_count / subdev_at ───────────────────────────── */

static void test_count_at(void)
{
    printf("--- test_count_at (T04 ①) ---\n");

    write_conf(CONF_2);
    assert(subdev_load(g_path, NULL, SUBDEVICE_MAX) == 2);
    assert(subdev_count() == 2);
    assert(subdev_at(0) != NULL);
    assert(subdev_at(1) != NULL);
    assert(strcmp(subdev_at(0)->device_id, "dev-mb-1") == 0);
    assert(strcmp(subdev_at(1)->device_id, "dev-mb-3") == 0);
    assert(subdev_at(-1) == NULL);
    assert(subdev_at(2) == NULL);
    printf("  count/at hit + OOB -> NULL: PASS\n");

    /* 空文件 → 0 条 */
    write_conf("# empty\n");
    assert(subdev_load(g_path, NULL, SUBDEVICE_MAX) == 0);
    assert(subdev_count() == 0);
    assert(subdev_at(0) == NULL);
    printf("  empty registry: count=0, at(0)=NULL: PASS\n");
}

/* ─── ② hw_subdev_init 对齐 ────────────────────────────────── */

static void test_init_alignment(void)
{
    printf("--- test_init_alignment (T04 ②) ---\n");

    struct node_config cfg;

    /* 合法 2 条 */
    write_conf(CONF_2);
    make_cfg(&cfg, g_path);
    assert(hw_subdev_init(&cfg) == 2);
    assert(hw_subdev_rt_count() == 2);
    for (int i = 0; i < 2; i++) {
        struct hw_subdev_stat st;
        assert(hw_subdev_stat_get(i, &st) == E_OK);
        assert(strcmp(st.device_id, subdev_at(i)->device_id) == 0);
        assert(st.registered == 0 && st.online == 0);
        assert(st.last_seen_ms == 0 && st.next_reg_ms == 0);
    }
    assert(hw_subdev_stat_get(-1, NULL) == E_INVAL);
    assert(hw_subdev_stat_get(0, NULL) == E_INVAL);
    {
        struct hw_subdev_stat st;
        assert(hw_subdev_stat_get(2, &st) == E_INVAL);
    }
    printf("  init 2 + rt[i].e aligned: PASS\n");

    /* 空文件 → 0 槽（纯网关） */
    write_conf("");
    make_cfg(&cfg, g_path);
    assert(hw_subdev_init(&cfg) == 0);
    assert(hw_subdev_rt_count() == 0);
    {
        struct hw_subdev_stat st;
        assert(hw_subdev_stat_get(0, &st) == E_INVAL);
    }
    printf("  empty -> 0 slot (gateway-only): PASS\n");

    /* NULL cfg */
    assert(hw_subdev_init(NULL) == E_INVAL);
    printf("  NULL cfg -> E_INVAL: PASS\n");
}

/* ─── ③ publish_data 返回码契约 ────────────────────────────── */

static void test_publish_data_contract(void)
{
    printf("--- test_publish_data_contract (T04 ③) ---\n");

    struct node_config cfg;
    write_conf(CONF_2);
    make_cfg(&cfg, g_path);
    assert(hw_subdev_init(&cfg) == 2);

    hw_subdev_set_publisher(stub_pub);
    g_pub_rc = E_OK;
    g_pub_calls = 0;

    struct sensor_data d;
    memset(&d, 0, sizeof(d));
    d.temperature = 12.34; d.humidity = 56.78; d.pressure = 1013.25;
    d.timestamp_ms = 1694793600000LL;

    /* 未命中注册表（modbus 99 未登记）→ E_OK（永久不可路由，已处置）*/
    d.source = SOURCE_MODBUS; d.source_id = 99;
    assert(platform_huawei_ops.publish_data(&cfg, &d) == E_OK);
    assert(g_pub_calls == 0);
    printf("  miss (unregistered) -> E_OK, no publish: PASS\n");

    /* v1.4.0 网关纯化：SOURCE_LOCAL 无注册来源（路由只认 modbus slave_id）
     * → 未命中丢弃（E_OK 且不发布），不得进入续传 */
    d.source = SOURCE_LOCAL; d.source_id = 0;
    d.temperature = 12.34; d.humidity = 56.78; d.pressure = 1013.25;
    assert(platform_huawei_ops.publish_data(&cfg, &d) == E_OK);
    assert(g_pub_calls == 0);
    printf("  SOURCE_LOCAL unmatched -> E_OK, no publish: PASS\n");

    /* 全空（三字段哨兵，modbus slave 3 命中）→ E_OK + empty_skip_cnt++ */
    d.source = SOURCE_MODBUS; d.source_id = 3;
    d.temperature = SENSOR_VALUE_INVALID;
    d.humidity    = SENSOR_VALUE_INVALID;
    d.pressure    = SENSOR_VALUE_INVALID;
    assert(platform_huawei_ops.publish_data(&cfg, &d) == E_OK);
    {
        struct hw_subdev_stat st;
        assert(hw_subdev_stat_get(1, &st) == E_OK);   /* rt[1]=dev-mb-3 */
        assert(st.empty_skip_cnt >= 1);
    }
    assert(g_pub_calls == 0);
    printf("  all-invalid -> E_OK, empty_skip_cnt++: PASS\n");

    /* 构造截断（1e308 x3）→ E_IO（非 E_OK，瞬态）*/
    d.temperature = 1e308; d.humidity = 1e308; d.pressure = 1e308;
    assert(platform_huawei_ops.publish_data(&cfg, &d) == E_IO);
    printf("  truncated build -> E_IO: PASS\n");

    /* 命中 + 发布成功 → E_OK，last_seen 刷新，补报 ONLINE */
    d.temperature = 12.34; d.humidity = 56.78; d.pressure = 1013.25;
    assert(platform_huawei_ops.publish_data(&cfg, &d) == E_OK);
    assert(g_pub_calls >= 1);
    {
        struct hw_subdev_stat st;
        assert(hw_subdev_stat_get(1, &st) == E_OK);
        assert(st.last_seen_ms != 0);
        assert(st.online == 1);
    }
    printf("  hit + publish ok -> E_OK, last_seen/online: PASS\n");

    /* modbus 命中（slave_id 3）→ E_OK */
    d.source = SOURCE_MODBUS; d.source_id = 3;
    assert(platform_huawei_ops.publish_data(&cfg, &d) == E_OK);
    printf("  modbus hit (slave 3) -> E_OK: PASS\n");

    /* 命中 + 发布失败（瞬态）→ E_NET（非 E_OK）*/
    g_pub_rc = E_NET;
    d.source = SOURCE_MODBUS; d.source_id = 3;
    assert(platform_huawei_ops.publish_data(&cfg, &d) == E_NET);
    printf("  hit + publish fail -> E_NET: PASS\n");

    /* 防御 */
    g_pub_rc = E_OK;
    assert(platform_huawei_ops.publish_data(NULL, &d) == E_INVAL);
    assert(platform_huawei_ops.publish_data(&cfg, NULL) == E_INVAL);
    printf("  NULL args -> E_INVAL: PASS\n");
}

/* ─── ④ publish_status / publish_alert ─────────────────────── */

static void test_status_alert(void)
{
    printf("--- test_status_alert (T04 ④) ---\n");

    struct node_config cfg;
    write_conf(CONF_2);
    make_cfg(&cfg, g_path);
    assert(hw_subdev_init(&cfg) == 2);

    hw_subdev_set_publisher(stub_pub);
    g_pub_rc = E_OK;

    struct device_info dev;
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.hostname, sizeof(dev.hostname), "gw-host");

    /* publish_status → properties/report + version/status */
    assert(platform_huawei_ops.publish_status(&cfg, &dev, "online") == E_OK);
    assert(strstr(g_last_topic, "/sys/properties/report") != NULL);
    assert(strstr(g_last_payload, "\"status\":\"online\"") != NULL);
    assert(strstr(g_last_payload, "\"version\":\"" EMBMQTTNODE_VERSION "\"") != NULL);
    printf("  publish_status -> properties/report: PASS\n");

    /* publish_alert：modbus 源 → source_id=slave_id 十进制 */
    struct alert_event evt;
    memset(&evt, 0, sizeof(evt));
    snprintf(evt.rule_name, sizeof(evt.rule_name), "r1");
    snprintf(evt.field, sizeof(evt.field), "temperature");
    evt.value = 88.5; evt.threshold = 80.0;
    snprintf(evt.source_kind, sizeof(evt.source_kind), "modbus");
    evt.source_id = 3;
    snprintf(evt.msg, sizeof(evt.msg), "temp high");
    evt.ts_ms = 1694793600000LL;

    assert(platform_huawei_ops.publish_alert(&cfg, &evt) == E_OK);
    assert(strstr(g_last_topic, "/sys/events/report") != NULL);
    assert(strstr(g_last_payload, "\"source_id\":\"3\"") != NULL);
    printf("  publish_alert(modbus) -> events/report: PASS\n");

    /* v1.4.0 网关纯化：source_kind 不再影响 source_id（一律数据源实例十进制）。
     * source_kind="sensor" 属保留死路径（alert_event.source_kind 保留）。 */
    snprintf(evt.source_kind, sizeof(evt.source_kind), "sensor");
    evt.source_id = 7;
    assert(platform_huawei_ops.publish_alert(&cfg, &evt) == E_OK);
    assert(strstr(g_last_payload, "\"source_id\":\"7\"") != NULL);
    printf("  publish_alert(sensor kind) -> source_id=decimal: PASS\n");
}

/* ─── ⑤ tick 退避单调性 ────────────────────────────────────── */

static void test_tick_backoff(void)
{
    printf("--- test_tick_backoff (T04 ⑤) ---\n");

    /* 纯函数：30s*n，300s 封顶，单调不减 */
    assert(hw_subdev_backoff_ms(0) == 30000);
    assert(hw_subdev_backoff_ms(1) == 30000);
    assert(hw_subdev_backoff_ms(2) == 60000);
    assert(hw_subdev_backoff_ms(10) == 300000);
    assert(hw_subdev_backoff_ms(11) == 300000);
    assert(hw_subdev_backoff_ms(1000) == 300000);
    for (int i = 1; i < 20; i++)
        assert(hw_subdev_backoff_ms(i + 1) >= hw_subdev_backoff_ms(i));
    printf("  backoff ms monotonic + 300s cap: PASS\n");

    /* 运行期：register 失败（桩返回 E_NET）→ reg_fail_cnt++、next_reg_ms 前移（门控） */
    struct node_config cfg;
    write_conf(CONF_2);
    make_cfg(&cfg, g_path);
    assert(hw_subdev_init(&cfg) == 2);

    hw_subdev_set_publisher(stub_pub);
    g_pub_rc = E_NET;   /* register 与 props 一律失败 */

    platform_huawei_ops.tick(&cfg);
    struct hw_subdev_stat st1;
    assert(hw_subdev_stat_get(0, &st1) == E_OK);
    assert(st1.reg_fail_cnt == 1);
    assert(st1.next_reg_ms > 0);

    /* 立即再 tick：now < next_reg_ms → 不再重试（退避门控生效） */
    platform_huawei_ops.tick(&cfg);
    struct hw_subdev_stat st2;
    assert(hw_subdev_stat_get(0, &st2) == E_OK);
    assert(st2.reg_fail_cnt == 1);
    assert(st2.next_reg_ms == st1.next_reg_ms);
    printf("  fail->cnt=1, backoff gate blocks re-retry: PASS\n");

    g_pub_rc = E_OK;
}

/* ─── ⑥ platform_select 装配 ───────────────────────────────── */

static void test_select_assembly(void)
{
    printf("--- test_select_assembly (T04 ⑥) ---\n");

    static struct node_config cfg;
    struct platform_connect_params p;

    write_conf(CONF_2);

    /* 凭据齐 + platform=huawei → huawei 激活（指纹：force_tls=1, use_will=0） */
    make_cfg(&cfg, g_path);
    snprintf(cfg.platform, sizeof(cfg.platform), "huawei");
    snprintf(cfg.huawei_device_id, sizeof(cfg.huawei_device_id), "gw-123");
    snprintf(cfg.huawei_secret, sizeof(cfg.huawei_secret), "secret-x");
    assert(platform_select(&cfg) == E_OK);
    assert(platform_connect_params(&cfg, &p) == E_OK);
    assert(p.force_tls == 1);
    assert(p.use_will == 0);
    assert(hw_subdev_rt_count() == 2);   /* hw_subdev_init 已随 select 执行 */
    printf("  creds ok -> huawei active (force_tls=1): PASS\n");

    /* 凭据缺 → 硬回落 local（指纹：force_tls=0, username==tls.username） */
    make_cfg(&cfg, g_path);
    snprintf(cfg.platform, sizeof(cfg.platform), "huawei");
    /* make_cfg 默认给了合法凭据，这里显式清空以构造「凭据非法」场景 */
    cfg.huawei_device_id[0] = '\0';
    cfg.huawei_secret[0]    = '\0';
    assert(platform_select(&cfg) == E_OK);
    assert(platform_connect_params(&cfg, &p) == E_OK);
    assert(p.force_tls == 0);
    assert(strcmp(p.username, cfg.tls.username) == 0);
    printf("  creds missing -> fallback local (force_tls=0): PASS\n");
}

/* ─── ⑦ on_message REGISTER_RESP ───────────────────────────── */

static void test_register_response(void)
{
    printf("--- test_register_response (T04 ⑦) ---\n");

    static struct node_config cfg;
    write_conf(CONF_2);
    make_cfg(&cfg, g_path);
    snprintf(cfg.platform, sizeof(cfg.platform), "huawei");
    snprintf(cfg.huawei_device_id, sizeof(cfg.huawei_device_id), "gw-123");
    snprintf(cfg.huawei_secret, sizeof(cfg.huawei_secret), "secret-x");
    assert(platform_select(&cfg) == E_OK);   /* 置 platform_node_config() */

    hw_subdev_set_publisher(stub_pub);
    g_pub_rc = E_OK;

    struct hw_subdev_stat st;
    assert(hw_subdev_stat_get(0, &st) == E_OK);
    assert(st.registered == 0);

    const char *topic =
        "$oc/devices/gw-123/sys/gateway/sub_devices/register/response/request_id=abc";
    const char *resp = "{\"devices\":[{\"device_id\":\"dev-mb-1\",\"status\":\"SUCCESS\"}]}";
    platform_huawei_ops.on_message(topic, resp, (int)strlen(resp));

    assert(hw_subdev_stat_get(0, &st) == E_OK);
    assert(st.registered == 1);
    assert(st.online == 1);   /* 补报 ONLINE（桩成功）*/
    printf("  REGISTER_RESP -> registered=1 + ONLINE: PASS\n");

    /* CMD_REQUEST 仅 seam：不执行、不崩溃 */
    const char *cmd =
        "$oc/devices/gw-123/sys/commands/request_id=xyz";
    const char *cmdp = "{\"command_name\":\"reboot\"}";
    platform_huawei_ops.on_message(cmd, cmdp, (int)strlen(cmdp));
    printf("  CMD_REQUEST (T05 seam) no-crash: PASS\n");

    /* OTHER 主题丢弃不崩 */
    platform_huawei_ops.on_message("some/other/topic", "x", 1);
    platform_huawei_ops.on_message(NULL, NULL, 0);
    printf("  OTHER/NULL no-crash: PASS\n");
}

/* ─── ⑧ T05 Q5：on_disconnected churn（flapping）计数 ──────── */

static void test_churn(void)
{
    printf("--- test_churn (T05 ⑧) ---\n");

    /* 首次断连：last==0 → 计数重置为 0 */
    platform_huawei_ops.on_disconnected();
    assert(hw_churn_count() == 0);

    /* 同窗口内连续断连 → 计数累计（相邻间隔 < 30s 视为 flapping） */
    for (int i = 0; i < 4; i++)
        platform_huawei_ops.on_disconnected();
    assert(hw_churn_count() >= 3);
    printf("  flapping count accumulates (>=3): PASS\n");
}

/* ─── 入口 ─────────────────────────────────────────────────── */

int main(void)
{
    printf("=== Platform Huawei Subdevice Tests (T04) ===\n\n");

    test_count_at();
    test_init_alignment();
    test_publish_data_contract();
    test_status_alert();
    test_tick_backoff();
    test_select_assembly();
    test_register_response();
    test_churn();

    hw_subdev_set_publisher(NULL);   /* 复位 */

    printf("\n=== ALL platform_huawei_subdev tests PASSED ===\n");
    return 0;
}
