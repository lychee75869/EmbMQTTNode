/*
 * platform_huawei.h / platform_huawei.c
 * 华为云 IoTDA 适配层——鉴权 / 主题 / payload builders（docs/12 §3.3）。
 *
 * 本轮（T03）只交付「纯函数 + 可单测」部分：鉴权现算、时间/主题、
 * $oc payload builders。**不**下沉 ops 表、**不**改 platform_select 的
 * huawei 分支（保持 T02 的 WARN + 回落 local）：
 *   - platform_huawei_ops 装配 + select 接线 → TODO(T04)
 *   - auth_type=1 重连监督线程 / mqtt_rebuild（±30min 超窗） → TODO(T05)
 *
 * 关键约束（docs/12 §1.3 / §4.1 / §8）：
 *   - 时间戳一律 gmtime_r 取 UTC（严禁 localtime）；type=0 亦取真实 UTC 时间
 *     （禁用占位/魔法常量：内嵌 ts 必须与 HMAC key 一致）。
 *   - password = HMAC-SHA256(key = ts_str, data = secret)，固定 64 位小写 hex
 *     （官方 p472/p523：以时间戳为密钥、对 secret 加密）。
 *   - payload 构造截断统一返回 E_IO 且拒绝发布；字段遍历走 §3.4 字段表。
 *   - 哨兵 mask 仅在 huawei builder 侧（local 不 mask，§3.5-4）。
 */
#ifndef PLATFORM_HUAWEI_H
#define PLATFORM_HUAWEI_H

#include "common.h"
#include "platform.h"          /* struct platform_connect_params */
#include "subdev_registry.h"   /* struct subdev_entry */

/*
 * hw_build_topic 的主题种类（§3.3）。
 * 说明：COMMANDS_RESPONSE 生成响应主题基址
 * `$oc/devices/{id}/sys/commands/response`；T05 回执时在其后追加
 * `/request_id={rid}`（本纯函数无 rid 入参，保持 §3.3 原型签名）。
 */
enum hw_topic {
    HW_TOPIC_PROPERTIES_REPORT = 0,  /* $oc/devices/{id}/sys/properties/report           */
    HW_TOPIC_BATCH,                  /* .../sys/gateway/sub_devices/properties/report    */
    HW_TOPIC_REGISTER,               /* .../sys/gateway/sub_devices/register             */
    HW_TOPIC_REGISTER_RESPONSE,      /* .../sys/gateway/sub_devices/register/response    */
    HW_TOPIC_STATUS,                 /* .../sys/gateway/sub_devices/status               */
    HW_TOPIC_COMMANDS_WILDCARD,      /* .../sys/commands/#                               */
    HW_TOPIC_EVENTS_REPORT,          /* .../sys/events/report                            */
    HW_TOPIC_COMMANDS_RESPONSE,      /* .../sys/commands/response                        */
    HW_TOPIC_COUNT,                  /* 哨兵法：越界 kind 一律 E_INVAL                    */
};

/* hw_classify_topic 的分类结果（§3.3 / §9-2） */
enum hw_topic_kind {
    HW_KIND_OTHER = 0,      /* 非关心的下行主题（不误判） */
    HW_KIND_CMD_REQUEST,    /* .../sys/commands/request_id={rid} */
    HW_KIND_REGISTER_RESP,  /* 前缀含 register + response（§9-2 容错匹配） */
};

/* ── 鉴权 ─────────────────────────────────────────────────────── */

/*
 * 拼装 client_id：`{device_id}_0_{auth_type}_{YYYYMMDDHH}`（UTC）。
 *   utc_tm 由调用方经 gmtime_r 取得（严禁 localtime）。
 *   len 调用方须传 ≥ 320（device_id 官方 String(256) + 后缀 15 + 裕量）；
 *   len < 320 或写入截断 → E_INVAL（fail-closed，不返回半截 id）。
 *   auth_type 仅接受 0 / 1，其他值 → E_INVAL。
 */
int hw_build_client_id(const char *device_id, int auth_type,
                       const struct tm *utc_tm, char *buf, int len);

/*
 * 拼装 password：HMAC-SHA256(key = ts_str, data = secret) → 固定 64 位小写 hex。
 *   ts_str 为 "YYYYMMDDHH"（与 client_id 内嵌时间戳同源）。
 *   len 须 ≥ 65（64 + NUL），否则 E_INVAL。
 *   secret / ts_str / hex_out 为 NULL → E_INVAL。
 */
int hw_build_password(const char *secret, const char *ts_str,
                      char *hex_out, int len);

/* keepalive 钳制到 30..1200（越界钳制 + 告警）。clamped 为 NULL → E_INVAL。 */
int hw_clamp_keepalive(int ka, int *clamped);

/* ── 时间 / 主题 ──────────────────────────────────────────────── */

/*
 * 事件时间：UTC `yyyyMMdd'T'HHmmss'Z'`（P1-2：重放/实发统一用数据自带
 * timestamp_ms）。len 须 ≥ 17，否则 E_INVAL。
 */
int hw_format_event_time(int64_t ts_ms, char *buf, int len);

/*
 * 拼装 $oc 主题（§3.3）。device_id 取 cfg->huawei_device_id；
 * cfg==NULL 或 device_id 空串 → E_INVAL；截断 → E_IO。
 */
int hw_build_topic(const struct node_config *cfg, enum hw_topic kind,
                   char *buf, int len);

/* 下行主题分类（NULL 或未命中 → HW_KIND_OTHER）。 */
enum hw_topic_kind hw_classify_topic(const char *topic);

/*
 * 从 `.../request_id={rid}`（或 register 响应同类主题）提取 rid 到 buf。
 *   未出现 "request_id=" → E_NOT_FOUND；值为空（畸形）→ E_INVAL；
 *   值放不下 len（拒绝而非截断）→ E_IO。
 */
int hw_extract_request_id(const char *topic, char *buf, int len);

/* ── JSON 字符串插值安全谓词（T05 R3，共享）────────────────────── */

/*
 * 判定 s 是否可安全地以 "%s" 插值进 JSON 字符串字面量：
 *   安全 = 不含 `"`、`\`、任意裸控制字符（< 0x20）。NULL → 0（不安全）；
 *   空串 → 1（安全）。供 hw_cmd_parse / hw_build_ota_status_event 等
 *   插值前 fail-closed 校验复用（拒绝而非转义）。
 */
int hw_json_str_safe(const char *s);

/* ── payload builders（截断拒绝：n<0 || n>=len → E_IO）────────── */

/*
 * 网关属性上报 payload（裁决 b：数据源 = device_info + 版本 + status）。
 * 不含 event_time（由平台按接收时间处理，保持纯函数无 time() 依赖）。
 */
int hw_build_gateway_props(const struct node_config *cfg,
                           const struct device_info *dev,
                           const char *status, char *buf, int len);

/*
 * 子设备批量上报 payload（方案 A，§3.5）：
 *   {"devices":[{"device_id":..,"services":[{"service_id":..,
 *     "properties":{ 值==SENSOR_VALUE_INVALID 的字段整体省略 },
 *     "event_time":hw_format_event_time(d->timestamp_ms)}]}]}
 *   三字段全被 mask → 返回特征码 E_NOT_FOUND（调用方跳过发布，不发空对象）。
 *   字段遍历走 §3.4 字段表（for_each_field），不再写 strcmp。
 */
int hw_build_batch_report(const struct subdev_entry *e,
                          const struct sensor_data *d, char *buf, int len);

/* 子设备注册 payload。 */
int hw_build_subdev_register(const struct subdev_entry *e, char *buf, int len);

/* 子设备上下线状态 payload（online!=0 → "ONLINE"，否则 "OFFLINE"）。 */
int hw_build_subdev_status(const struct subdev_entry *e, int online,
                           char *buf, int len);

/* 告警 → 物模型事件 payload（P1-1；schema 集中于本单点，§9-3）。 */
int hw_build_event_alert(const struct alert_event *evt, const char *source_id,
                         char *buf, int len);

/* 命令回执 payload（result_code / result_msg / request_id）。 */
int hw_build_command_response(const char *request_id, int result_code,
                              const char *result_msg, char *buf, int len);

/*
 * 华为平台连接参数装配（纯函数，供 T04 的 platform_huawei_ops 使用）。
 *   现算 ts（gmtime_r UTC）→ client_id / HMAC password / username=device_id；
 *   keepalive 钳制；use_will=0；force_tls=1；ca_file=cfg->huawei_ca_file；
 *   rebuild_on_hour=(auth_type==1)；built_ts=生成 client_id 的 UTC epoch。
 *   device_id / secret 为空 → E_INVAL（供 select 回落判定）。
 */
int hw_connect_params(const struct node_config *cfg,
                      struct platform_connect_params *out);

/* ── 子设备管理运行期（platform_huawei_subdev.c，T04）────────────── */

/*
 * 装配子设备运行期状态：加载注册表（subdev_load）并按 subdev_count()/
 * subdev_at() 构建 g_subdev_rt[] 对齐表（注册表条目只有一份事实源）。
 * 必须在任何线程创建之前（platform_select 内）调用一次。
 *   返回: 成功加载的条目数（≥0）；cfg==NULL → E_INVAL。
 * 注意：注册表 0 条**不**返回错误——这是合法的「纯网关」模式（打醒目 WARN）。
 */
int hw_subdev_init(const struct node_config *cfg);

/* 运行期槽数量（= hw_subdev_init 对齐的条目数；只读，线程创建后不变） */
int hw_subdev_rt_count(void);

/* 运行期子设备状态快照（只读；供单测/诊断，不改变运行语义） */
struct hw_subdev_stat {
    char    device_id[SUBDEV_DEVICE_ID_LEN];
    int     registered;
    int     online;
    int     reg_fail_cnt;
    int     empty_skip_cnt;
    int64_t last_seen_ms;   /* CLOCK_MONOTONIC 毫秒 */
    int64_t next_reg_ms;    /* CLOCK_MONOTONIC 毫秒 */
};
/* 返回 idx 槽快照；越界（idx<0 || idx>=hw_subdev_rt_count()）→ E_INVAL */
int hw_subdev_stat_get(int idx, struct hw_subdev_stat *out);

/*
 * 注册重试退避（纯函数，可单测）：min(30s * fail_cnt, 300s)。
 * fail_cnt<=0 视为 1。单调不减、300s 封顶。
 */
int64_t hw_subdev_backoff_ms(int fail_cnt);

/*
 * 可注入发布后端（默认 mqtt_publish_raw）。仅供单测注入桩，使
 * 「publish_data 命中 → E_OK」等返回码契约可在**无 broker**下单测；
 * 生产路径 g_pub==NULL → 恒用 mqtt_publish_raw。传 NULL 复位。
 */
typedef int (*hw_pub_fn)(const char *topic, const char *payload, int qos);
void hw_subdev_set_publisher(hw_pub_fn fn);

/* ── T05：命令闭环 / OTA 状态 shim / churn（platform_huawei_cmd.c）────── */

/*
 * 下行命令处理（平台命令闭环）。由 platform_huawei_subdev.c 的
 * hw_on_message 在 HW_KIND_CMD_REQUEST 分支**一行委派**（Q1 seam）。
 * 运行于 libmosquitto 网络线程：内部仅「解析 + 同步 QoS1 回执」；
 * ota_handle_message 只改状态不做 I/O，reboot 不在网络线程内等待 → 不阻塞。
 */
void hw_cmd_handle(const char *topic, const char *payload, int len);

/*
 * OTA 状态 → 物模型事件 shim（**非 static**：platform.c 的
 * platform_ota_status_publish 在 huawei 模式下调用它）。
 * 签名与 ota_set_mqtt_publish 回调一致；把 OTA 状态 JSON 解析后重建为
 * ota_status 事件，发到 $oc/.../sys/events/report（QoS1）。其输出经
 * mqtt_publish_raw → 不回灌本 shim（无递归）。
 */
int hw_ota_status_shim(const char *topic, const char *payload, int qos);

/*
 * OTA 状态事件 payload builder（纯函数；schema 单点，§9-3）：
 *   {"services":[{"service_id":"Gateway","event_type":"ota_status",
 *     "event_time":"<UTC>","paras":{"state":"<s>","version":"<v>"}}]}
 *   now_ms 取 publish 时刻（OTA 状态是实时事件，非历史采样）。
 */
int hw_build_ota_status_event(const char *state, const char *version,
                              int64_t now_ms, char *buf, int len);

/*
 * 可注入发布后端（默认 mqtt_publish_raw）。仅供单测注入桩，使
 * 命令回执 / OTA 事件在**无 broker**下可断言（与 hw_subdev_set_publisher
 * 同理由）；生产路径 g_cmd_pub==NULL → 恒用 mqtt_publish_raw。传 NULL 复位。
 */
void hw_cmd_set_publisher(hw_pub_fn fn);

/*
 * churn（flapping）观察计数：mono_ms 30s 窗口内连续断连累计（诊断/单测）。
 * hw_on_disconnected 维护；只读，不改变协议行为（§8）。
 */
int hw_churn_count(void);

#endif /* PLATFORM_HUAWEI_H */
