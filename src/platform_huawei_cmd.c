/*
 * platform_huawei_cmd.c
 * 华为云 IoTDA 命令闭环 + OTA 桥接 + OTA 状态 shim（docs/14 Q1/Q2/Q3）。
 *
 * 组成：
 *   - hw_cmd_handle()：下行命令处理（由 hw_on_message 的 CMD_REQUEST seam 委派）
 *       Q1 判定顺序 0~7：rid→command_name→paras 校验→busy→桥接→回执；
 *       Q2 OTA 桥接：受控 builder 重建扁平旧 schema → ota_handle_message；
 *       Q6 reboot：先回执后停机（platform_reboot_request）。
 *   - hw_ota_status_shim()：OTA 状态 JSON → ota_status 物模型事件（Q3）；
 *   - hw_build_ota_status_event()：事件 payload 纯函数 builder。
 *
 * 关键纪律：
 *   - 运行于 libmosquitto 网络线程：禁止阻塞/sleep；回执 QoS1 同步发出；
 *     不放任何等待（flush 由 main 既有退出窗口承担）。
 *   - ota.c 零改动：受理判定复用既有公开 API ota_state_string()
 *     的「前置 + 调用后立即后置」双观测（TOCTOU 窗口最小化：返回后零插入）。
 *   - 不新增错误码；不改 platform_ops 9 成员签名。
 *   - payload 带长度（非 NUL）：先 memcpy 到本地 NUL 缓冲（len>=N 拒绝）。
 */
#include "platform_huawei.h"
#include "mqtt_client.h"
#include "ota.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* 本地 JSON 缓冲：与 ota.c 的 OTA_JSON_BUF_SIZE 对齐（该宏为 ota.c 内部，
 * 此处以同值本地常量表达；len>=该值拒绝而非截断）。 */
#define HW_CMD_BUF_SIZE  2048

/* ─── 可注入发布后端（默认 mqtt_publish_raw；供单测桩）────────── */

static hw_pub_fn g_cmd_pub = NULL;

void hw_cmd_set_publisher(hw_pub_fn fn)
{
    g_cmd_pub = fn;
}

static int cmd_pub(const char *topic, const char *payload, int qos)
{
    if (g_cmd_pub)
        return g_cmd_pub(topic, payload, qos);
    return mqtt_publish_raw(topic, payload, qos);
}

/* ─── 解析结果 ─────────────────────────────────────────────── */

enum hw_cmd_kind {
    HW_CMD_UNKNOWN = 0,
    HW_CMD_OTA_UPGRADE,
    HW_CMD_REBOOT,
};

struct hw_cmd_parsed {
    enum hw_cmd_kind kind;
    char cmd[16];
    char version[OTA_VERSION_MAX];
    char url[OTA_URL_MAX];
    char checksum[OTA_CHECKSUM_MAX];
};

/*
 * 判定某键在 JSON 中的「字符串值」状态（仅用于 checksum 的缺省/非字符串区分）：
 *   0 = 键不存在（或未处于键位置）
 *   1 = 键存在，且其值为带引号的字符串
 *   2 = 键存在，但值不是字符串（数字/布尔/对象/数组）
 * 与 json_get_string 的键位置判定同口径（左侧最近非空白字符为 '{' 或 ','）。
 */
static int key_str_status(const char *json, size_t len, const char *key)
{
    size_t klen = strlen(key);
    if (klen == 0 || klen > 64)
        return 0;

    char kq[70];
    kq[0] = '"';
    memcpy(kq + 1, key, klen);
    kq[klen + 1] = '"';

    for (size_t i = 0; i + klen + 2 <= len; i++) {
        if (memcmp(json + i, kq, klen + 2) != 0)
            continue;

        int at_key_pos = 0;
        for (size_t j = i; j > 0; ) {
            j--;
            char c = json[j];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                continue;
            at_key_pos = (c == '{' || c == ',');
            break;
        }
        if (!at_key_pos)
            continue;

        size_t p = i + klen + 2;
        while (p < len && (json[p] == ' ' || json[p] == '\t' ||
                           json[p] == '\n' || json[p] == '\r'))
            p++;
        if (p >= len || json[p] != ':')
            continue;
        p++;
        while (p < len && (json[p] == ' ' || json[p] == '\t' ||
                           json[p] == '\n' || json[p] == '\r'))
            p++;
        if (p >= len)
            continue;
        return (json[p] == '"') ? 1 : 2;
    }
    return 0;
}

/*
 * 命令完整性解析（全字符串，§9-6）。返回：
 *   0 = OTA 或 reboot 命令解析成功（out->kind 有效）
 *   1 = ota_upgrade 的 paras 校验失败（→ 回执 code=1）
 *   2 = command_name 缺失/无法解析/不识别（→ 回执 code=2）
 */
static int hw_cmd_parse(const char *buf, size_t len, struct hw_cmd_parsed *out)
{
    memset(out, 0, sizeof(*out));

    char command_name[32];
    if (!json_get_string(buf, len, "command_name",
                         command_name, sizeof(command_name)))
        return 2;   /* 缺失/非字符串/超长 → 未知命令 */

    if (strcmp(command_name, "reboot") == 0) {
        out->kind = HW_CMD_REBOOT;
        return 0;
    }
    if (strcmp(command_name, "ota_upgrade") != 0)
        return 2;   /* 不认识 → 未知命令 */

    out->kind = HW_CMD_OTA_UPGRADE;

    /* cmd（ota_upgrade 内）：必填，须精确 "upgrade" */
    if (!json_get_string(buf, len, "cmd", out->cmd, sizeof(out->cmd)))
        return 1;
    if (strcmp(out->cmd, "upgrade") != 0)
        return 1;

    /* version：必填非空 + JSON 插值安全（T05 R1：含 " \ 或控制符 → 拒绝） */
    if (!json_get_string(buf, len, "version",
                         out->version, sizeof(out->version)))
        return 1;
    if (out->version[0] == '\0')
        return 1;
    if (!hw_json_str_safe(out->version))
        return 1;

    /* url：必填非空 + JSON 插值安全（T05 R1） */
    if (!json_get_string(buf, len, "url", out->url, sizeof(out->url)))
        return 1;
    if (out->url[0] == '\0')
        return 1;
    if (!hw_json_str_safe(out->url))
        return 1;

    /* checksum：可缺省（→空；后续 VERIFYING 必 mismatch → FAILED，fail-closed）；
     * 存在但非字符串 / 超长 / 畸形 → code=1；JSON 插值安全（T05 R1，空串安全） */
    int cs = key_str_status(buf, len, "checksum");
    if (cs == 0) {
        out->checksum[0] = '\0';
    } else if (cs == 2) {
        return 1;
    } else {
        if (!json_get_string(buf, len, "checksum",
                             out->checksum, sizeof(out->checksum)))
            return 1;
    }
    if (!hw_json_str_safe(out->checksum))
        return 1;

    return 0;
}

/* ─── 回执发送（Q1-(4)/(5)）────────────────────────────────── */

static void send_response(const struct node_config *cfg, const char *rid,
                          int code, const char *msg)
{
    char base[256], topic[320], payload[256];

    /* T05 审计修复：rid 来自下行 topic（远程可达），插值进回执 JSON 前
     * fail-closed 校验（含 " \ 或控制符 → 丢弃，不回执） */
    if (!hw_json_str_safe(rid)) {
        LOG_WARN("huawei cmd: unsafe request_id — no reply");
        return;
    }

    if (hw_build_topic(cfg, HW_TOPIC_COMMANDS_RESPONSE,
                       base, sizeof(base)) != E_OK) {
        LOG_WARN("huawei cmd: build response topic base failed");
        return;
    }

    /* 两步拼接 + 显式截断检查（拒绝而非截断） */
    int n = snprintf(topic, sizeof(topic), "%s/request_id=%s", base, rid);
    if (n < 0 || n >= (int)sizeof(topic)) {
        LOG_WARN("huawei cmd: response topic overflow — refusing to send");
        return;
    }

    if (hw_build_command_response(rid, code, msg,
                                  payload, sizeof(payload)) != E_OK) {
        LOG_WARN("huawei cmd: build response payload failed");
        return;
    }

    /* QoS1 同步回执（网络线程内 publish 为 libmosquitto 允许用法） */
    if (cmd_pub(topic, payload, 1) != E_OK)
        LOG_WARN("huawei cmd: publish response failed (code=%d)", code);
}

/* ─── 命令闭环入口（network thread）─────────────────────────── */

void hw_cmd_handle(const char *topic, const char *payload, int len)
{
    if (!topic || !payload)
        return;

    const struct node_config *cfg = platform_node_config();
    if (!cfg) {
        LOG_WARN("huawei cmd: no node_config injected — dropping command");
        return;
    }

    /* (3)0：rid 提取失败 → 不处理、不回执、丢弃（无法关联来源） */
    char rid[64];
    if (hw_extract_request_id(topic, rid, sizeof(rid)) != E_OK) {
        LOG_WARN("huawei cmd: cannot extract request_id from '%s' — dropping",
                 topic);
        return;
    }

    /* payload 拷贝到本地 NUL 缓冲（len>=N 拒绝，不截断） */
    if (len < 0 || len >= HW_CMD_BUF_SIZE) {
        LOG_WARN("huawei cmd: payload too large (%d >= %d) — rejecting",
                 len, HW_CMD_BUF_SIZE);
        send_response(cfg, rid, 1, "payload too large");
        return;
    }
    char buf[HW_CMD_BUF_SIZE];
    memcpy(buf, payload, (size_t)len);
    buf[len] = '\0';

    struct hw_cmd_parsed p;
    int pr = hw_cmd_parse(buf, (size_t)len, &p);

    if (pr == 2) {
        LOG_WARN("huawei cmd: unknown command (rid=%s)", rid);
        send_response(cfg, rid, 2, "unknown command");
        return;
    }
    if (pr == 1) {
        LOG_WARN("huawei cmd: invalid ota_upgrade parameters (rid=%s)", rid);
        send_response(cfg, rid, 1, "invalid parameters");
        return;
    }

    /* (3)6：reboot —— 先回执，后停机（顺序不得颠倒） */
    if (p.kind == HW_CMD_REBOOT) {
        send_response(cfg, rid, 0, "rebooting");
        void (*reboot_cb)(void) = platform_reboot_request();
        if (reboot_cb)
            reboot_cb();
        return;
    }

    /* (3)3：前置 busy 检测 */
    if (strcmp(ota_state_string(), "idle") != 0) {
        LOG_WARN("huawei cmd: ota busy, rejecting upgrade (rid=%s)", rid);
        send_response(cfg, rid, 1, "device busy");
        return;
    }

    /* Q2：受控 builder 重建扁平旧 schema（不透传 huawei envelope） */
    char json[2048];
    int n = snprintf(json, sizeof(json),
        "{\"cmd\":\"upgrade\",\"version\":\"%s\",\"url\":\"%s\","
        "\"checksum\":\"%s\"}",
        p.version, p.url, p.checksum);
    if (n < 0 || n >= (int)sizeof(json)) {
        send_response(cfg, rid, 1, "upgrade rejected");
        return;
    }

    /* (3)4/5：同步受理 → **返回后立即**后置复核（中间零插入，抑制 TOCTOU） */
    ota_handle_message(json, n);
    int accepted = (strcmp(ota_state_string(), "idle") != 0);

    if (accepted) {
        LOG_INFO("huawei cmd: upgrade accepted (rid=%s)", rid);
        send_response(cfg, rid, 0, "upgrade accepted");
    } else {
        /* 仍 idle = 被拒（disabled / pubkey 未配置 / 解析拒） */
        LOG_WARN("huawei cmd: upgrade rejected (rid=%s)", rid);
        send_response(cfg, rid, 1, "upgrade rejected");
    }
}

/* ─── Q3：OTA 状态 → 事件 ──────────────────────────────────── */

int hw_build_ota_status_event(const char *state, const char *version,
                              int64_t now_ms, char *buf, int len)
{
    if (!state || !buf || len <= 0)
        return E_INVAL;

    /* T05 R2：state/version 亦属远程可达输入（命令 paras.version → ota 配置
     * → ota_report_status → shim 再解析），插值前 fail-closed 校验 */
    const char *ver = version ? version : "";
    if (!hw_json_str_safe(state) || !hw_json_str_safe(ver))
        return E_INVAL;

    char et[32];
    int rc = hw_format_event_time(now_ms, et, (int)sizeof(et));
    if (rc != E_OK)
        return rc;

    int n = snprintf(buf, (size_t)len,
        "{\"services\":[{\"service_id\":\"Gateway\",\"event_type\":\"ota_status\","
        "\"event_time\":\"%s\",\"paras\":{\"state\":\"%s\",\"version\":\"%s\"}}]}",
        et, state, ver);
    if (n < 0 || n >= len)
        return E_IO;
    return E_OK;
}

int hw_ota_status_shim(const char *topic, const char *payload, int qos)
{
    /* OTA 原始状态主题（本地 ota/status）仅作触发源；事件改发 $oc events/report */
    (void)topic;

    if (!payload)
        return E_INVAL;

    const struct node_config *cfg = platform_node_config();
    if (!cfg)
        return E_INVAL;

    size_t len = strlen(payload);

    char state[32];
    if (!json_get_string(payload, len, "state", state, sizeof(state))) {
        LOG_WARN("huawei ota shim: cannot parse 'state' from payload");
        return E_INVAL;
    }

    char version[OTA_VERSION_MAX];
    version[0] = '\0';
    (void)json_get_string(payload, len, "version",
                          version, sizeof(version));   /* 可选 */

    char etopic[256];
    if (hw_build_topic(cfg, HW_TOPIC_EVENTS_REPORT,
                       etopic, sizeof(etopic)) != E_OK)
        return E_IO;

    char evt[512];
    int rc = hw_build_ota_status_event(state, version,
                                       (int64_t)time(NULL) * 1000,
                                       evt, (int)sizeof(evt));
    if (rc != E_OK) {
        LOG_WARN("huawei ota shim: build event failed (%d)", rc);
        return rc;
    }

    /* QoS1；经 mqtt_publish_raw → 不回灌本 shim（无递归） */
    return cmd_pub(etopic, evt, qos > 0 ? qos : 1);
}
