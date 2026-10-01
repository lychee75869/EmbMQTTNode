/*
 * platform_huawei.c
 * 华为云 IoTDA 适配层——鉴权 / 主题 / payload builders。
 *
 * 纯函数实现（无全局状态、无 I/O、可单测）。所有 $oc 主题与 payload 的
 * 组装集中于此；字段遍历统一走字段表（SENSOR_FIELDS）。
 *
 * 哨兵 mask：仅在本文件的 huawei builder 生效——值
 * == SENSOR_VALUE_INVALID 的字段整体不写入 properties；local 路径不 mask。
 */
#include "platform_huawei.h"
#include "sensor_fields.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>

/* ─── 内部：累加写入（截断 → E_IO）──────────────────────────── */

/*
 * 从 buf[pos] 起按 fmt 追加；成功返回新位置（>=0），截断返回 E_IO（<0）。
 * 语义与「payload 构造截断统一返回 E_IO（n<0 || n>=len）」一致。
 */
#if defined(__GNUC__)
__attribute__((format(printf, 4, 5)))
#endif
static int ap(char *buf, int len, int pos, const char *fmt, ...)
{
    if (pos < 0 || pos >= len)
        return E_IO;

    va_list va;
    va_start(va, fmt);
    int n = vsnprintf(buf + pos, (size_t)(len - pos), fmt, va);
    va_end(va);

    if (n < 0 || n >= len - pos)
        return E_IO;
    return pos + n;
}

/* ─── 0. JSON 字符串插值安全谓词 ───────────────────────────── */

/*
 * 判定字符串 s 是否可安全地以 "%s" 形式插值进 JSON 字符串字面量。
 * 安全 = 不含双引号、反斜杠、任意裸控制字符（字节值 < 0x20，含 \n \r \t）；
 * 其余字节（含空格 0x20、UTF-8 ≥0x80 多字节）均可直接原样写入。
 *   - NULL → 0（不安全，避免空指针/漏判）
 *   - 空串 → 1（安全：无可插值内容）
 * 设计为 fail-closed 拒绝依据：调用方在插值前校验，不安全即拒绝（不回退转义）。
 */
int hw_json_str_safe(const char *s)
{
    if (!s)
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c < 0x20)          /* 裸控制字符：JSON 规范禁止 */
            return 0;
        if (c == '"' || c == '\\')
            return 0;
    }
    return 1;
}

/*
 * payload 构造器的 JSON 字符串插值守卫。
 * 不安全值（含 " \ 或裸控制符）替换为空串 ""——保持输出 JSON 恒合法、
 * 不丢整条上报，并打 WARN（字段名可见，便于定位配置源头）。
 * 安全值原样返回：正常字符串的输出逐字节不变（golden 兼容）。
 * 拒绝而非转义；因 device_info/alert_event 属本地源
 * 字段（无「丢整条」语义），「拒绝」在此退化为置空该字段。
 */
static const char *hw_json_field(const char *field_name, const char *v)
{
    if (hw_json_str_safe(v))
        return v;
    LOG_WARN("huawei: %s contains unsafe JSON chars, reported as empty string",
             field_name);
    return "";
}

/* ─── 1. 鉴权 ─────────────────────────────────────────────────── */

int hw_build_client_id(const char *device_id, int auth_type,
                       const struct tm *utc_tm, char *buf, int len)
{
    if (!device_id || !utc_tm || !buf)
        return E_INVAL;
    if (auth_type != 0 && auth_type != 1)
        return E_INVAL;
    /* device_id 官方 String(256) + 后缀 "_0_{auth}_{YYYYMMDDHH}"(≤15) + 裕量 */
    if (len < 320)
        return E_INVAL;

    int n = snprintf(buf, (size_t)len, "%s_0_%d_%04d%02d%02d%02d",
                     device_id, auth_type,
                     utc_tm->tm_year + 1900, utc_tm->tm_mon + 1,
                     utc_tm->tm_mday, utc_tm->tm_hour);
    if (n < 0 || n >= len)
        return E_INVAL;   /* 不应发生（已校验 len<320），保守 fail-closed */
    return E_OK;
}

int hw_build_password(const char *secret, const char *ts_str,
                      char *hex_out, int len)
{
    if (!secret || !ts_str || !hex_out)
        return E_INVAL;
    if (len < 65)   /* 固定 64 hex + NUL */
        return E_INVAL;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;

    /* 官方钉死：key = 时间戳(ts_str)，data = secret；固定 64 位小写 hex */
    if (HMAC(EVP_sha256(), ts_str, (int)strlen(ts_str),
             (const unsigned char *)secret, strlen(secret),
             digest, &dlen) == NULL)
        return E_NET;
    if (dlen != 32)   /* SHA-256 恒 32 字节；否则视为异常 */
        return E_NET;

    static const char hexd[] = "0123456789abcdef";
    for (unsigned int i = 0; i < dlen; i++) {
        hex_out[2 * i]     = hexd[(digest[i] >> 4) & 0xF];
        hex_out[2 * i + 1] = hexd[digest[i] & 0xF];
    }
    hex_out[dlen * 2] = '\0';
    return E_OK;
}

int hw_clamp_keepalive(int ka, int *clamped)
{
    if (!clamped)
        return E_INVAL;

    if (ka < 30) {
        LOG_WARN("huawei keepalive %d < 30, clamped to 30", ka);
        *clamped = 30;
    } else if (ka > 1200) {
        LOG_WARN("huawei keepalive %d > 1200, clamped to 1200", ka);
        *clamped = 1200;
    } else {
        *clamped = ka;
    }
    return E_OK;
}

/* ─── 2. 时间 / 主题 ──────────────────────────────────────────── */

int hw_format_event_time(int64_t ts_ms, char *buf, int len)
{
    if (!buf || len < 17)   /* "yyyyMMddTHHmmssZ" = 16 + NUL */
        return E_INVAL;

    time_t sec = (time_t)(ts_ms / 1000);
    struct tm utc;
    if (gmtime_r(&sec, &utc) == NULL)   /* 严禁 localtime */
        return E_INVAL;

    int n = snprintf(buf, (size_t)len, "%04d%02d%02dT%02d%02d%02dZ",
                     utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                     utc.tm_hour, utc.tm_min, utc.tm_sec);
    if (n < 0 || n >= len)
        return E_IO;
    return E_OK;
}

int hw_build_topic(const struct node_config *cfg, enum hw_topic kind,
                   char *buf, int len)
{
    if (!cfg || !buf || len <= 0)
        return E_INVAL;
    if (cfg->huawei_device_id[0] == '\0')
        return E_INVAL;

    const char *fmt;
    switch (kind) {
    case HW_TOPIC_PROPERTIES_REPORT:
        fmt = "$oc/devices/%s/sys/properties/report"; break;
    case HW_TOPIC_BATCH:
        fmt = "$oc/devices/%s/sys/gateway/sub_devices/properties/report"; break;
    case HW_TOPIC_REGISTER:
        fmt = "$oc/devices/%s/sys/gateway/sub_devices/register"; break;
    case HW_TOPIC_REGISTER_RESPONSE:
        fmt = "$oc/devices/%s/sys/gateway/sub_devices/register/response"; break;
    case HW_TOPIC_STATUS:
        fmt = "$oc/devices/%s/sys/gateway/sub_devices/status"; break;
    case HW_TOPIC_COMMANDS_WILDCARD:
        fmt = "$oc/devices/%s/sys/commands/#"; break;
    case HW_TOPIC_EVENTS_REPORT:
        fmt = "$oc/devices/%s/sys/events/report"; break;
    case HW_TOPIC_COMMANDS_RESPONSE:
        fmt = "$oc/devices/%s/sys/commands/response"; break;
    default:
        return E_INVAL;
    }

    int n = snprintf(buf, (size_t)len, fmt, cfg->huawei_device_id);
    if (n < 0 || n >= len)
        return E_IO;
    return E_OK;
}

enum hw_topic_kind hw_classify_topic(const char *topic)
{
    if (!topic)
        return HW_KIND_OTHER;

    /* register 响应按「前缀含 register + response」容错匹配 */
    if (strstr(topic, "register") && strstr(topic, "response"))
        return HW_KIND_REGISTER_RESP;

    /* 命令请求：.../sys/commands/request_id={rid}
     * 注意 commands/response 主题含 "response"，不匹配本模式 → OTHER（不误判） */
    if (strstr(topic, "/sys/commands/request_id="))
        return HW_KIND_CMD_REQUEST;

    return HW_KIND_OTHER;
}

int hw_extract_request_id(const char *topic, char *buf, int len)
{
    if (!topic || !buf || len <= 0)
        return E_INVAL;

    const char *p = strstr(topic, "request_id=");
    if (!p)
        return E_NOT_FOUND;
    p += strlen("request_id=");

    size_t vlen = 0;
    while (p[vlen] != '\0' && p[vlen] != '&')
        vlen++;

    if (vlen == 0)                       /* 畸形：空 rid */
        return E_INVAL;
    if (vlen >= (size_t)len)             /* 放不下：拒绝而非截断 */
        return E_IO;

    memcpy(buf, p, vlen);
    buf[vlen] = '\0';
    return E_OK;
}

/* ─── 3. payload builders ─────────────────────────────────────── */

int hw_build_gateway_props(const struct node_config *cfg,
                           const struct device_info *dev,
                           const char *status, char *buf, int len)
{
    if (!cfg || !dev || !status || !buf || len <= 0)
        return E_INVAL;

    int pos = ap(buf, len, 0,
        "{\"services\":[{\"service_id\":\"Gateway\",\"properties\":{"
        "\"version\":\"%s\",\"status\":\"%s\",\"hostname\":\"%s\","
        "\"mac\":\"%s\",\"cpu\":\"%s\",\"kernel\":\"%s\",\"mem_kb\":%lld}}]}",
        EMBMQTTNODE_VERSION, status,
        hw_json_field("hostname", dev->hostname),
        hw_json_field("mac_addr", dev->mac_addr),
        hw_json_field("cpu_model", dev->cpu_model),
        hw_json_field("kernel_ver", dev->kernel_ver),
        (long long)dev->total_mem_kb);

    return (pos < 0) ? E_IO : E_OK;
}

int hw_build_batch_report(const struct subdev_entry *e,
                          const struct sensor_data *d, char *buf, int len)
{
    if (!e || !d || !buf || len <= 0)
        return E_INVAL;

    char et[32];
    int rc = hw_format_event_time(d->timestamp_ms, et, (int)sizeof(et));
    if (rc != E_OK)
        return rc;

    int pos = ap(buf, len, 0,
        "{\"devices\":[{\"device_id\":\"%s\",\"services\":[{\"service_id\":\"%s\","
        "\"properties\":{",
        e->device_id, e->service_id);
    if (pos < 0)
        return E_IO;

    /* 值 == SENSOR_VALUE_INVALID 的字段整体省略；
     * 字段遍历走字段表，不写 strcmp。 */
    int any = 0;
    for_each_field(f) {
        double v = *(const double *)((const char *)d + f->offset);
        if (v == SENSOR_VALUE_INVALID)
            continue;
        pos = ap(buf, len, pos, "%s\"%s\":%.2f", any ? "," : "", f->name, v);
        if (pos < 0)
            return E_IO;
        any = 1;
    }

    /* 三字段全空 → properties 为 {}，返回特征码，调用方跳过发布（不发空对象） */
    if (!any)
        return E_NOT_FOUND;

    pos = ap(buf, len, pos, "},\"event_time\":\"%s\"}]}]}", et);
    if (pos < 0)
        return E_IO;
    return E_OK;
}

int hw_build_subdev_register(const struct subdev_entry *e, char *buf, int len)
{
    if (!e || !buf || len <= 0)
        return E_INVAL;

    int pos = ap(buf, len, 0,
        "{\"devices\":[{\"device_id\":\"%s\",\"name\":\"%s\"}]}",
        e->device_id, e->name);

    return (pos < 0) ? E_IO : E_OK;
}

int hw_build_subdev_status(const struct subdev_entry *e, int online,
                           char *buf, int len)
{
    if (!e || !buf || len <= 0)
        return E_INVAL;

    int pos = ap(buf, len, 0,
        "{\"devices\":[{\"device_id\":\"%s\",\"status\":\"%s\"}]}",
        e->device_id, online ? "ONLINE" : "OFFLINE");

    return (pos < 0) ? E_IO : E_OK;
}

int hw_build_event_alert(const struct alert_event *evt, const char *source_id,
                         char *buf, int len)
{
    if (!evt || !buf || len <= 0)
        return E_INVAL;

    char et[32];
    int rc = hw_format_event_time(evt->ts_ms, et, (int)sizeof(et));
    if (rc != E_OK)
        return rc;

    int pos = ap(buf, len, 0,
        "{\"services\":[{\"service_id\":\"Gateway\",\"event_type\":\"alert\","
        "\"event_time\":\"%s\",\"paras\":{"
        "\"rule_name\":\"%s\",\"field\":\"%s\",\"value\":%.2f,"
        "\"threshold\":%.2f,\"source\":\"%s\",\"source_id\":\"%s\","
        "\"msg\":\"%s\"}}]}",
        et,
        hw_json_field("rule_name", evt->rule_name),
        hw_json_field("field", evt->field),
        evt->value, evt->threshold,
        hw_json_field("source_kind", evt->source_kind),
        source_id ? source_id : "",
        hw_json_field("msg", evt->msg));

    return (pos < 0) ? E_IO : E_OK;
}

int hw_build_command_response(const char *request_id, int result_code,
                              const char *result_msg, char *buf, int len)
{
    if (!buf || !result_msg || len <= 0)
        return E_INVAL;

    int pos = ap(buf, len, 0,
        "{\"result_code\":%d,\"result_msg\":\"%s\",\"request_id\":\"%s\"}",
        result_code, result_msg, request_id ? request_id : "");

    return (pos < 0) ? E_IO : E_OK;
}

/* ─── 4. 连接参数装配（纯函数）─────────────────────────────── */

int hw_connect_params(const struct node_config *cfg,
                      struct platform_connect_params *out)
{
    if (!cfg || !out)
        return E_INVAL;

    /* 供 select 回落判定：device_id / secret 为空即不可用 */
    if (cfg->huawei_device_id[0] == '\0' || cfg->huawei_secret[0] == '\0')
        return E_INVAL;

    memset(out, 0, sizeof(*out));

    /* ts 现算：gmtime_r UTC（严禁 localtime；type=0 亦取真实 UTC 时间，
     * 内嵌 ts 必须与 HMAC key 一致，与 type=1 共用同一代码路径） */
    time_t now = time(NULL);
    struct tm utc;
    if (gmtime_r(&now, &utc) == NULL)
        return E_NET;

    char ts[16];
    int n = snprintf(ts, sizeof(ts), "%04d%02d%02d%02d",
                     utc.tm_year + 1900, utc.tm_mon + 1,
                     utc.tm_mday, utc.tm_hour);
    if (n < 0 || n >= (int)sizeof(ts))
        return E_IO;

    if (hw_build_client_id(cfg->huawei_device_id, cfg->huawei_auth_type,
                           &utc, out->client_id, (int)sizeof(out->client_id))
        != E_OK)
        return E_INVAL;

    snprintf(out->username, sizeof(out->username), "%s", cfg->huawei_device_id);

    if (hw_build_password(cfg->huawei_secret, ts,
                          out->password, (int)sizeof(out->password)) != E_OK)
        return E_INVAL;

    int ka = 0;
    hw_clamp_keepalive(cfg->huawei_keepalive, &ka);
    out->keepalive = ka;

    out->force_tls = 1;     /* 华为 8883 强制 TLS（不依赖用户 tls_enabled） */
    out->use_will = 0;      /* 平台不支持遗嘱 */
    out->rebuild_on_hour = (cfg->huawei_auth_type == 1) ? 1 : 0;
    out->built_ts = (int)now;

    snprintf(out->ca_file, sizeof(out->ca_file), "%s", cfg->huawei_ca_file);
    /* will_topic / will_payload 留空（use_will=0，mqtt_init 不设置遗嘱） */

    return E_OK;
}
