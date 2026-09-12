/*
 * tests/test_mqtt_client.c
 * MQTT 客户端订阅生命周期单元测试（v1.2.8 P1-13 + P2-19 — 第 8 个测试）
 *
 * libmosquitto 的 on_connect 依赖真实 broker（CONNACK 报文驱动），
 * 无法在单元测试环境直接覆盖。本测试不硬造 broker，而是覆盖本轮
 * 修复特意抽出的两条可纯函数化链路：
 *
 *   1. mqtt_handle_connack —— on_connect 的函数体（CONNACK 处理）：
 *      - 回调注册后 rc==0 必须触发回调（P2-19：online/订阅由事件驱动）
 *      - 重复触发（模拟断网重连后再次 CONNACK 成功）必须再次回调
 *        （P1-13 核心：重连后订阅恢复依赖回调被再次调用）
 *      - 未注册回调时不得崩溃（判空保护）
 *      - rc!=0 时不得触发回调且连接标志清零
 *      - 回调清除（NULL）后不得再触发
 *   2. mqtt_build_ota_topic / mqtt_build_status_topic —— 主题构造：
 *      - 格式正确（订阅/topic 一致性是断网续传与告警上报的前提）
 *      - 参数防御（NULL / buf_len<=0 → E_INVAL）
 *      - 小缓冲安全截断（不越界、保证 null 结尾）
 *
 * 未覆盖路径（需集成环境，手动验证步骤见文件尾注释）：
 *   - on_connect 与 libmosquitto 网络线程的挂接（mqtt_init 全链路）
 *   - 回调内 mosquitto_publish/subscribe 的真实网络行为
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "../src/mqtt_client.h"
#include "../src/common.h"   /* E_OK / E_INVAL / E_NET */

/* ─── 回调触发计数器（模拟"重订阅 + 重发状态"副作用）────── */

static int g_cb_count = 0;

static void counting_callback(void) {
    g_cb_count++;
}

/* ─── mqtt_handle_connack：回调调度逻辑 ─────────────────── */

/* 未注册回调时 CONNACK 成功不得崩溃，且连接标志置位 */
static void test_connack_no_callback(void) {
    printf("--- test_connack_no_callback ---\n");
    mqtt_set_connected_callback(NULL);
    g_cb_count = 0;

    int rc = mqtt_handle_connack(0);
    assert(rc == E_OK);
    assert(g_cb_count == 0);
    assert(mqtt_is_connected() == 1);
    printf("  no cb, rc==0, no crash:     PASS\n");
}

/* P2-19 核心：注册回调后，CONNACK 成功必须触发回调 */
static void test_connack_fires_callback(void) {
    printf("--- test_connack_fires_callback (P2-19) ---\n");
    mqtt_set_connected_callback(counting_callback);
    g_cb_count = 0;

    int rc = mqtt_handle_connack(0);
    assert(rc == E_OK);
    assert(g_cb_count == 1);
    assert(mqtt_is_connected() == 1);
    printf("  registered cb fired once:   PASS\n");
}

/*
 * P1-13 核心回归：模拟断网重连——第二次 CONNACK 成功必须再次触发
 * 回调。旧实现订阅只在 main 启动时做一次，重连后（clean session 下
 * broker 已清订阅）OTA 通道永久失效；新实现靠本回调每次重订。
 */
static void test_connack_reconnect_refires(void) {
    printf("--- test_connack_reconnect_refires (P1-13) ---\n");
    mqtt_set_connected_callback(counting_callback);
    g_cb_count = 0;

    /* 首次连接成功 */
    assert(mqtt_handle_connack(0) == E_OK);
    assert(g_cb_count == 1);

    /* 断连（on_disconnect 置 g_connected=0 的路径由 rc!=0 等价模拟：
     * 连接失败/拒绝同样走清零分支） */
    assert(mqtt_handle_connack(3) == E_NET);
    assert(mqtt_is_connected() == 0);

    /* 重连成功：必须再次触发回调 */
    assert(mqtt_handle_connack(0) == E_OK);
    assert(g_cb_count == 2);
    assert(mqtt_is_connected() == 1);
    printf("  reconnect fires cb again:   PASS\n");
}

/* rc!=0（连接被拒）时不得触发回调，且连接标志清零 */
static void test_connack_refused_no_fire(void) {
    printf("--- test_connack_refused_no_fire ---\n");
    mqtt_set_connected_callback(counting_callback);
    g_cb_count = 0;

    assert(mqtt_handle_connack(5) == E_NET);
    assert(g_cb_count == 0);
    assert(mqtt_is_connected() == 0);
    printf("  rc!=0 no fire, flag=0:      PASS\n");
}

/* 清除回调（NULL）后 CONNACK 成功不得再触发 */
static void test_callback_cleared(void) {
    printf("--- test_callback_cleared ---\n");
    mqtt_set_connected_callback(counting_callback);
    mqtt_set_connected_callback(NULL);   /* 清除 */
    g_cb_count = 0;

    assert(mqtt_handle_connack(0) == E_OK);
    assert(g_cb_count == 0);
    printf("  cleared cb not fired:       PASS\n");
}

/* ─── 主题构造纯函数 ───────────────────────────────────── */

static void test_build_ota_topic(void) {
    printf("--- test_build_ota_topic ---\n");
    char topic[256];

    assert(mqtt_build_ota_topic("emb-node-aabbcc", topic,
                                (int)sizeof(topic)) == E_OK);
    assert(strcmp(topic, "embmqttnode/emb-node-aabbcc/ota/cmd") == 0);
    printf("  format embmqttnode/%%s/ota/cmd: PASS\n");
}

static void test_build_status_topic(void) {
    printf("--- test_build_status_topic ---\n");
    char topic[256];

    assert(mqtt_build_status_topic("emb/node/data", topic,
                                   (int)sizeof(topic)) == E_OK);
    assert(strcmp(topic, "emb/node/data/status") == 0);
    printf("  format %%s/status:            PASS\n");
}

static void test_build_topic_defensive(void) {
    printf("--- test_build_topic_defensive ---\n");
    char topic[256];

    assert(mqtt_build_ota_topic(NULL, topic, (int)sizeof(topic)) == E_INVAL);
    assert(mqtt_build_ota_topic("id", NULL, (int)sizeof(topic)) == E_INVAL);
    assert(mqtt_build_ota_topic("id", topic, 0) == E_INVAL);
    assert(mqtt_build_ota_topic("id", topic, -1) == E_INVAL);

    assert(mqtt_build_status_topic(NULL, topic, (int)sizeof(topic)) == E_INVAL);
    assert(mqtt_build_status_topic("t", NULL, (int)sizeof(topic)) == E_INVAL);
    assert(mqtt_build_status_topic("t", topic, 0) == E_INVAL);
    printf("  NULL/len<=0 -> E_INVAL:     PASS\n");
}

/* 小缓冲：snprintf 安全截断，不越界且保证 null 结尾 */
static void test_build_topic_truncate(void) {
    printf("--- test_build_topic_truncate ---\n");
    char buf[16];

    memset(buf, 0xAA, sizeof(buf));   /* 哨兵预填，验证不越界 */
    assert(mqtt_build_ota_topic("emb-node-aabbcc", buf,
                                (int)sizeof(buf)) == E_OK);
    /* "embmqttnode/" 12 字符 + "emb" = 15 字符，恰好填满 buf[0..14]，
     * buf[15]='\0'，buf[15] 原哨兵 0xAA 被覆盖为 '\0' */
    assert(strcmp(buf, "embmqttnode/emb") == 0);
    assert(buf[15] == '\0');
    printf("  truncation null-terminated:  PASS\n");
}

/* ─── 入口 ─────────────────────────────────────────────── */

int main(void) {
    printf("=== MQTT Client Subscription Lifecycle Tests "
           "(v1.2.8 P1-13/P2-19) ===\n\n");

    test_connack_no_callback();
    test_connack_fires_callback();
    test_connack_reconnect_refires();
    test_connack_refused_no_fire();
    test_callback_cleared();

    test_build_ota_topic();
    test_build_status_topic();
    test_build_topic_defensive();
    test_build_topic_truncate();

    printf("\n=== ALL mqtt_client tests PASSED ===\n");
    return 0;
}

/*
 * ─── 集成验证步骤（无法单测的路径，供联调/验收参考）────────
 *
 * 1. 启动 mosquitto（本机或容器），配置 node.conf 指向它；
 * 2. `mosquitto_sub -t 'embmqttnode/+/ota/cmd' -t '+/status' -v` 观察；
 * 3. 启动 embmqttnode：应看到 online 状态与 ota 订阅（日志
 *    "ota topic subscribed"），且无需依赖任何启动延时；
 * 4. P1-13 回归：`systemctl stop mosquitto` 再 start（或断网/恢复），
 *    等待自动重连后再次向 embmqttnode/<id>/ota/cmd 发消息，
 *    网关必须仍能收到并触发升级流程；同时 status 主题应重发 online；
 * 5. P2-19 回归：在 broker 上开启慢认证（或用 iptables 延迟 CONNACK
 *    > 0.5s），旧实现会静默丢订阅；新实现应不受影响。
 */
