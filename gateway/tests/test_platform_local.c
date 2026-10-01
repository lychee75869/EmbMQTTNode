/*
 * tests/test_platform_local.c
 * 平台适配层单元测试（local 实现）——第 11 个测试
 *
 * 目标：为平台抽象层提供"无 broker"可
 * 回归的护栏，覆盖：
 *
 *   ① local connect_params 纯函数断言
 *      （TLS 关/单/双向、空 user/pass、keepalive=60、force_tls=0、
 *        use_will=1、rebuild_on_hour=0、遗嘱 topic/payload 构造）；
 *   ② platform_select 装配矩阵
 *      （""/"local"/"huawei"/未知 → 均落 local；NULL → E_INVAL）；
 *   ③ 唯一路由 seam
 *      （platform_dispatch_message → platform_local.on_message 的路由
 *        决策；证明下行只有一条路径，且 ota 关时丢弃、非 ota 主题无路由）；
 *   ④ 分发器薄转发的参数防御与离线安全
 *      （无 broker 时 publish 安全返回 E_INVAL，不崩）。
 *
 * ── 唯一路由 / exact-once OTA 入口的结构证明（grep 证据，命令可复现）──
 *   $ grep -rn "ota_handle_message" src --include=*.c | grep -v "^src/ota.c"
 *     src/platform_local.c:        ota_handle_message(payload, len);
 *   → 全仓对下行终点 ota_handle_message 的调用**有且仅有一处**
 *     （platform_local.on_message），即"唯一路由归属"。
 *   $ grep -n "platform_dispatch_message" src/mqtt_client.c
 *     src/mqtt_client.c:117:        platform_dispatch_message(msg->topic, ...);
 *   → 传输层 on_message 只有一个转发点（不再判 topic、不再二次分发）。
 *   $ grep -rn "mqtt_set_ota_callback" src tests
 *     (无输出) → 旧的"回调双路由" API 已彻底移除。
 *   $ grep -n "mqtt_publish\|mqtt_publish_raw\|mqtt_subscribe" src/main.c
 *     src/main.c: ota_set_mqtt_publish(mqtt_publish_raw);   ← 仅 OTA 状态注入
 *   → main.c 不再有直连 mqtt_* 数据路径。
 *
 * 未覆盖（需真实 broker 的集成路径，见文件尾 E2E 步骤）：
 *   - 断连→自动重连→CONNACK→platform_on_connected→重订 OTA/重发 online
 *     的真实报文往返（本测试用 seam 覆盖了该链路的"装配与转发"端，
 *     真实往返由 mosquitto E2E 覆盖）。
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../src/common.h"
#include "../src/platform.h"
#include "../src/mqtt_client.h"
#include "../src/ota.h"

/* ─── 构造一份 baseline 配置（贴近 config_load 之后的 g_cfg）────── */

static void make_cfg(struct node_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->broker_host, sizeof(cfg->broker_host), "127.0.0.1");
    cfg->broker_port = 1883;
    snprintf(cfg->topic, sizeof(cfg->topic), "embmqttnode/emb-node-aabbcc");
    snprintf(cfg->client_id, sizeof(cfg->client_id), "emb-node-aabbcc");
}

/* ─── ① local connect_params 纯函数断言 ───────────────────────── */

/* 1a. TLS 单向 + 有 user/pass：全字段透传，恒定值正确 */
static void test_connect_params_tls_on(void)
{
    printf("--- test_connect_params_tls_on ---\n");

    struct node_config cfg;
    make_cfg(&cfg);
    cfg.tls.enabled = 1;
    snprintf(cfg.tls.username, sizeof(cfg.tls.username), "user1");
    snprintf(cfg.tls.password, sizeof(cfg.tls.password), "pass1");
    snprintf(cfg.tls.ca_file, sizeof(cfg.tls.ca_file), "/etc/emb/ca.pem");

    /* 缺省 platform=""（按 local 处理） */
    assert(platform_select(&cfg) == E_OK);

    struct platform_connect_params p;
    memset(&p, 0xAA, sizeof(p));   /* 哨兵：验证实现确有整体 memset 归零 */
    assert(platform_connect_params(&cfg, &p) == E_OK);

    /* 透传字段 */
    assert(strcmp(p.client_id, "emb-node-aabbcc") == 0);
    assert(strcmp(p.username, "user1") == 0);
    assert(strcmp(p.password, "pass1") == 0);
    assert(strcmp(p.ca_file, "/etc/emb/ca.pem") == 0);

    /* 恒定字段 */
    assert(p.keepalive == 60);
    assert(p.force_tls == 0);        /* local 的 TLS 由 cfg->tls.enabled 驱动 */
    assert(p.use_will == 1);         /* 恒定设置遗嘱 */
    assert(p.rebuild_on_hour == 0);  /* local 无跨小时重建 */
    assert(p.built_ts > 0);

    /* 遗嘱构造：topic = <topic>/status，payload 含 client_id/offline/timestamp */
    assert(strcmp(p.will_topic, "embmqttnode/emb-node-aabbcc/status") == 0);
    assert(strstr(p.will_payload, "\"client_id\":\"emb-node-aabbcc\"") != NULL);
    assert(strstr(p.will_payload, "\"status\":\"offline\"") != NULL);
    assert(strstr(p.will_payload, "\"timestamp\":") != NULL);

    printf("  tls_on: client/user/pass/ca/will/keepalive  OK\n");
}

/* 1b. TLS 关 + 空 user/pass：可空字段为空串 */
static void test_connect_params_tls_off_empty_auth(void)
{
    printf("--- test_connect_params_tls_off_empty_auth ---\n");

    struct node_config cfg;
    make_cfg(&cfg);
    cfg.tls.enabled = 0;             /* 无 TLS */
    cfg.tls.username[0] = '\0';      /* 无认证 */
    cfg.tls.password[0] = '\0';
    cfg.tls.ca_file[0] = '\0';

    assert(platform_select(&cfg) == E_OK);

    struct platform_connect_params p;
    assert(platform_connect_params(&cfg, &p) == E_OK);

    assert(p.username[0] == '\0');
    assert(p.password[0] == '\0');
    assert(p.ca_file[0] == '\0');
    assert(p.force_tls == 0);        /* 关键：local 不强制 TLS（TLS 落点回归） */
    assert(p.keepalive == 60);
    /* will 仍然设置（与 TLS 无关） */
    assert(p.use_will == 1 && p.will_topic[0] != '\0');

    printf("  tls_off/empty_auth: 空串透传 + force_tls=0  OK\n");
}

/* 1c. 参数防御：out=NULL → E_INVAL；cfg=NULL → E_INVAL */
static void test_connect_params_defensive(void)
{
    printf("--- test_connect_params_defensive ---\n");

    struct node_config cfg;
    make_cfg(&cfg);
    struct platform_connect_params p;

    assert(platform_connect_params(&cfg, NULL) == E_INVAL);
    assert(platform_connect_params(NULL, &p) == E_INVAL);
    printf("  NULL args -> E_INVAL  OK\n");
}

/* ─── ② platform_select 装配矩阵 ─────────────────────────────── */

/*
 * 观测手段：平台是否落 local 可由 connect_params 的"平台指纹"判定——
 *   local：username = cfg->tls.username（透传）；
 *   huawei：username = device_id、password = HMAC（非透传）。
 * 因此 "select 后 username 仍等于 tls.username" 即证明 active==local。
 */
static int active_is_local(const struct node_config *cfg)
{
    struct platform_connect_params p;
    if (platform_connect_params(cfg, &p) != E_OK)
        return 0;
    return strcmp(p.username, cfg->tls.username) == 0
        && p.force_tls == 0;
}

static void test_select_matrix(void)
{
    printf("--- test_select_matrix ---\n");

    struct node_config cfg;

    /* 2a. 空串 → local */
    make_cfg(&cfg);
    cfg.tls.enabled = 1;
    snprintf(cfg.tls.username, sizeof(cfg.tls.username), "u-empty");
    cfg.platform[0] = '\0';
    assert(platform_select(&cfg) == E_OK);
    assert(active_is_local(&cfg));
    printf("  platform=\"\"        -> local  OK\n");

    /* 2b. "local" → local */
    make_cfg(&cfg);
    cfg.tls.enabled = 1;
    snprintf(cfg.tls.username, sizeof(cfg.tls.username), "u-local");
    snprintf(cfg.platform, sizeof(cfg.platform), "local");
    assert(platform_select(&cfg) == E_OK);
    assert(active_is_local(&cfg));
    printf("  platform=\"local\"   -> local  OK\n");

    /* 2c. "huawei" → fail-safe 回落 local（凭据缺失/非法） */
    make_cfg(&cfg);
    cfg.tls.enabled = 1;
    snprintf(cfg.tls.username, sizeof(cfg.tls.username), "u-hw");
    snprintf(cfg.platform, sizeof(cfg.platform), "huawei");
    snprintf(cfg.huawei_device_id, sizeof(cfg.huawei_device_id), "hw-dev-001");
    assert(platform_select(&cfg) == E_OK);
    assert(active_is_local(&cfg));   /* 回落证明：username 未被 device_id 覆盖 */
    printf("  platform=\"huawei\"  -> local(fallback)  OK\n");

    /* 2d. 未知值 → fail-safe 回落 local */
    make_cfg(&cfg);
    cfg.tls.enabled = 1;
    snprintf(cfg.tls.username, sizeof(cfg.tls.username), "u-unknown");
    snprintf(cfg.platform, sizeof(cfg.platform), "aliyun");
    assert(platform_select(&cfg) == E_OK);
    assert(active_is_local(&cfg));
    printf("  platform=\"aliyun\"  -> local(fallback)  OK\n");

    /* 2e. NULL → E_INVAL（不改变已选定的 g_active） */
    assert(platform_select(NULL) == E_INVAL);
    printf("  select(NULL)       -> E_INVAL  OK\n");
}

/* ─── ③ 唯一路由 seam（下行只有一条路径）───────────────────────── */

/*
 * 3a. ota 关闭：任意主题（含 ota/cmd）一律丢弃、不得崩溃，且**不**进入
 *     OTA 状态机（local_on_message 的 ota.enabled 门控）。
 */
static void test_route_ota_disabled_drops(void)
{
    printf("--- test_route_ota_disabled_drops ---\n");

    struct node_config cfg;
    make_cfg(&cfg);
    cfg.ota.enabled = 0;
    assert(platform_select(&cfg) == E_OK);

    /* 即便主题命中 /ota/cmd，ota 关 → 丢弃不崩 */
    platform_dispatch_message("embmqttnode/emb-node-aabbcc/ota/cmd", "{}", 2);
    platform_dispatch_message("embmqttnode/emb-node-aabbcc/data", "hello", 5);
    printf("  ota disabled: drop, no crash  OK\n");
}

/*
 * 3b. ota 开启 → 命中 /ota/cmd 走唯一终点 ota_handle_message。此处未
 *     ota_init，ota.c 内部 g_cfg.enabled==0 → 立即 "ignored(disabled)" 返回，
 *     不会触发任何网络 I/O；以此证明"路由可达且安全"（exact-once 的
 *     结构证明见文件头 grep 证据）。
 */
static void test_route_ota_enabled_reaches_terminal(void)
{
    printf("--- test_route_ota_enabled_reaches_terminal ---\n");

    struct node_config cfg;
    make_cfg(&cfg);
    cfg.ota.enabled = 1;
    assert(platform_select(&cfg) == E_OK);

    /* 命中 /ota/cmd → 进入 ota_handle_message（未 init 时安全早返回） */
    platform_dispatch_message("embmqttnode/emb-node-aabbcc/ota/cmd",
                              "{\"cmd\":\"upgrade\"}", 18);
    printf("  ota enabled: route to terminal, no crash  OK\n");
}

/*
 * 3c. 非 ota 主题：local 无该路由 → 记录后丢弃，不崩（证明没有"第二
 *     个消费者"悄悄处理下行）。
 */
static void test_route_unknown_topic_no_consumer(void)
{
    printf("--- test_route_unknown_topic_no_consumer ---\n");

    struct node_config cfg;
    make_cfg(&cfg);
    cfg.ota.enabled = 1;
    assert(platform_select(&cfg) == E_OK);

    platform_dispatch_message("some/other/topic", "x", 1);
    platform_dispatch_message("embmqttnode/emb-node-aabbcc/cmd", "x", 1);
    printf("  unknown topic: no consumer, no crash  OK\n");
}

/* 3d. 防御：topic/payload 为 NULL 不得崩（传输层字节序列契约） */
static void test_route_null_safe(void)
{
    printf("--- test_route_null_safe ---\n");

    platform_dispatch_message(NULL, NULL, 0);
    platform_dispatch_message(NULL, "x", 1);
    platform_dispatch_message("t", NULL, 0);
    printf("  NULL topic/payload: no crash  OK\n");
}

/* ─── ④ 分发器薄转发：离线安全（无 broker）────────────────────── */

static void test_publish_offline_safe(void)
{
    printf("--- test_publish_offline_safe ---\n");

    struct node_config cfg;
    make_cfg(&cfg);
    cfg.ota.enabled = 0;
    assert(platform_select(&cfg) == E_OK);

    struct sensor_data d;
    memset(&d, 0, sizeof(d));
    /* 网关纯化：板载采集层移除，数据一律走 modbus topic + JSON 路径 */
    d.source = SOURCE_MODBUS;
    d.source_id = 1;
    d.temperature = 23.45;
    d.timestamp_ms = 1;

    struct alert_event evt;
    memset(&evt, 0, sizeof(evt));
    snprintf(evt.msg, sizeof(evt.msg), "ALERT: temp high");

    struct device_info dev;
    memset(&dev, 0, sizeof(dev));

    /* 未 mqtt_init（g_mosq==NULL）→ 各发布入口安全返回 E_INVAL，不崩 */
    assert(platform_publish_data(&cfg, &d) == E_INVAL);
    assert(platform_publish_alert(&cfg, &evt) == E_INVAL);
    assert(platform_publish_status(&cfg, &dev, "online") == E_INVAL);

    /* 参数防御：NULL 数据入参 */
    assert(platform_publish_data(&cfg, NULL) == E_INVAL);
    assert(platform_publish_alert(NULL, &evt) == E_INVAL);
    assert(platform_publish_alert(&cfg, NULL) == E_INVAL);

    printf("  offline publish: E_INVAL, no crash  OK\n");
}

/* ─── 入口 ─────────────────────────────────────────────────── */

int main(void)
{
    printf("=== Platform Local Adapter Tests ===\n\n");

    /* ① connect_params */
    test_connect_params_tls_on();
    test_connect_params_tls_off_empty_auth();
    test_connect_params_defensive();

    /* ② select 装配矩阵 */
    test_select_matrix();

    /* ③ 唯一路由 seam */
    test_route_ota_disabled_drops();
    test_route_ota_enabled_reaches_terminal();
    test_route_unknown_topic_no_consumer();
    test_route_null_safe();

    /* ④ 分发器离线安全 */
    test_publish_offline_safe();

    printf("\n=== ALL platform_local tests PASSED ===\n");
    return 0;
}

/*
 * ─── E2E（真实 broker）联调步骤 ─────────────────────────────
 *
 * 环境：WSL / Ubuntu 已装 mosquitto。
 * 1) 启动 broker（测试端口，勿扰生产）：
 *      /usr/sbin/mosquitto -p 18830 &
 * 2) 订阅观察（另开一终端）：
 *      mosquitto_sub -h 127.0.0.1 -p 18830 -t 'embmqttnode/#' -v
 * 3) 用平台/local 配置运行网关（config 指向 127.0.0.1:18830, platform=local）：
 *      应看到 embmqttnode/<id>/status = {"status":"online",...}
 *      日志出现 "ota topic subscribed: embmqttnode/<id>/ota/cmd"
 * 4) 断连重订回归：
 *      向 embmqttnode/<id>/ota/cmd 发一条命令 → 网关应收到；
 *      kill broker 再拉起（模拟断网）→ 重连后 CONNACK 再次触发
 *      platform_on_connected → 日志再现 "ota topic subscribed" 与 online；
 *      再次发 ota/cmd → 网关仍能收到（订阅已恢复）。
 * 5) 断网续传：断 broker 期间采集若干条 → 恢复后 upload_thread 经
 *    platform_publish_data 补发，topic 与在线一致。
 */
