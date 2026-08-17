/*
 * http_server.c
 * 最小化 HTTP/1.0 服务器，零外部依赖（阶段五）
 *
 * 功能：
 *   - 内嵌 HTML 仪表盘（单页应用，CSS Grid + 原生 JS）
 *   - REST JSON API（status / data / rules / ota）
 *   - 传感器数据环形缓冲区（128 条，供 dashboard 历史查询）
 *   - POST /api/reboot 设备重启
 *
 * 设计：
 *   - 阻塞 accept + 每连接一个请求（HTTP/1.0 语义）
 *   - 线程安全：环形缓冲区用 mutex 保护
 *   - JSON 响应使用 snprintf 栈上构造
 */

#include "http_server.h"
#include "storage.h"
#include "mqtt_client.h"
#include "rule_engine.h"
#include "anomaly_engine.h"
#include "ota.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>

/* ═══════════════════════════════════════════════════════════════
 * 常量
 * ═══════════════════════════════════════════════════════════════ */

#define HTTP_PORT_DEFAULT      8080
#define HTTP_BACKLOG           8
#define HTTP_RECV_BUF          2048
#define HTTP_SEND_BUF          8192
#define HTTP_MAX_PATH          256

#define RING_BUF_SIZE          128

/* ═══════════════════════════════════════════════════════════════
 * 全局状态
 * ═══════════════════════════════════════════════════════════════ */

static volatile int    g_running = 0;
static int             g_listen_fd = -1;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t         g_start_time_ms = 0;

/* 只读引用（由主线程在启动前设置） */
static const struct device_info *g_dev = NULL;
static const struct node_config  *g_cfg = NULL;

/* 环形缓冲区（最新数据在 head-1，最老数据在 head） */
static struct sensor_data g_ring[RING_BUF_SIZE];
static int                g_ring_head  = 0;
static int                g_ring_count = 0;

/* ═══════════════════════════════════════════════════════════════
 * 环形缓冲区操作（调用方需持锁）
 * ═══════════════════════════════════════════════════════════════ */

static void ring_push(const struct sensor_data *d)
{
    g_ring[g_ring_head] = *d;
    g_ring_head = (g_ring_head + 1) % RING_BUF_SIZE;
    if (g_ring_count < RING_BUF_SIZE)
        g_ring_count++;
}

/* 从最新到最旧拷贝 n 条到 out，返回实际拷贝数 */
static int ring_get_recent(struct sensor_data *out, int n)
{
    if (n > g_ring_count)
        n = g_ring_count;
    for (int i = 0; i < n; i++) {
        int idx = (g_ring_head - 1 - i + RING_BUF_SIZE) % RING_BUF_SIZE;
        out[i] = g_ring[idx];
    }
    return n;
}

/* 获取最新一条，返回 1=成功 0=无数据 */
static int ring_get_latest(struct sensor_data *out)
{
    if (g_ring_count == 0) return 0;
    int idx = (g_ring_head - 1 + RING_BUF_SIZE) % RING_BUF_SIZE;
    *out = g_ring[idx];
    return 1;
}

/* ═══════════════════════════════════════════════════════════════
 * HTTP 工具
 * ═══════════════════════════════════════════════════════════════ */

/*
 * 从 fd 读取一行（以 \r\n 结尾），去掉行尾的 \r\n。
 * 返回接收字节数，0=连接关闭，-1=错误
 */
static int http_read_line(int fd, char *buf, int buf_len)
{
    int total = 0;
    while (total < buf_len - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return (int)n;
        if (c == '\n') {
            /* 去掉前一个 \r（如果有） */
            if (total > 0 && buf[total - 1] == '\r')
                total--;
            break;
        }
        buf[total++] = c;
    }
    buf[total] = '\0';
    return total;
}

/*
 * 读取请求头，提取 Content-Length 值。
 * 读到空行 \r\n 为止，返回 Content-Length（未出现则返回 0）。
 */
static int http_read_headers(int fd)
{
    char buf[256];
    int content_length = 0;
    while (http_read_line(fd, buf, sizeof(buf)) > 0) {
        if (buf[0] == '\0') break;  /* 空行 = 头部结束 */
        if (strncmp(buf, "Content-Length:", 15) == 0) {
            content_length = atoi(buf + 15);
        } else if (strncmp(buf, "content-length:", 15) == 0) {
            content_length = atoi(buf + 15);
        }
    }
    return content_length;
}

/*
 * 发送 HTTP 响应
 */
static void http_send(int fd, int code, const char *content_type,
                      const char *body, int body_len)
{
    char hdr[512];
    int hdr_len = snprintf(hdr, sizeof(hdr),
                           "HTTP/1.0 %d %s\r\n"
                           "Content-Type: %s\r\n"
                           "Content-Length: %d\r\n"
                           "Connection: close\r\n"
                           "Access-Control-Allow-Origin: *\r\n"
                           "\r\n",
                           code,
                           code == 200 ? "OK" :
                           code == 400 ? "Bad Request" :
                           code == 404 ? "Not Found" :
                           code == 405 ? "Method Not Allowed" :
                           code == 500 ? "Internal Server Error" : "Unknown",
                           content_type,
                           body_len);

    send(fd, hdr, hdr_len, MSG_NOSIGNAL);
    if (body && body_len > 0)
        send(fd, body, body_len, MSG_NOSIGNAL);
}

static void http_send_json(int fd, int code, const char *json)
{
    http_send(fd, code, "application/json; charset=utf-8",
              json, (int)strlen(json));
}

static void http_send_error(int fd, int code, const char *msg)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    http_send_json(fd, code, buf);
}

/*
 * 从请求行解析 HTTP 方法和路径
 * 请求行格式: GET /path HTTP/1.0
 */
static int http_parse_request_line(const char *line,
                                   char *method, int mlen,
                                   char *path, int plen)
{
    /* sscanf 安全解析 */
    (void)mlen;
    (void)plen;
    int n = 0;
    if (sscanf(line, "%31s %255s %*s%n", method, path, &n) < 2)
        return -1;
    return 0;
}

/*
 * 从 query string 解析整数参数: ?n=100 → 100
 * 无参数时返回默认值
 */
static int parse_query_int(const char *path, const char *key, int default_val)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *p = strstr(path, pattern);
    if (!p) return default_val;
    p += strlen(pattern);
    int val = atoi(p);
    return (val > 0) ? val : default_val;
}

/*
 * 从 socket 精确读取 content_length 字节到 body 缓冲区
 */
static int http_read_body(int fd, char *body, int blen, int content_length)
{
    if (content_length <= 0 || content_length >= blen)
        return -1;
    int total = 0;
    while (total < content_length) {
        ssize_t n = recv(fd, body + total, content_length - total, 0);
        if (n <= 0) return -1;
        total += (int)n;
    }
    body[total] = '\0';
    return total;
}

/* ═══════════════════════════════════════════════════════════════
 * API 处理函数
 * ═══════════════════════════════════════════════════════════════ */

/* GET /api/status */
static void handle_api_status(int fd)
{
    int64_t now_ms = time(NULL) * 1000LL;
    int64_t uptime_s = 0;
    int64_t start_ms = g_start_time_ms;
    if (now_ms > start_ms)
        uptime_s = (now_ms - start_ms) / 1000LL;

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{"
             "\"client_id\":\"%s\","
             "\"version\":\"%s\","
             "\"uptime_seconds\":%lld,"
             "\"mqtt_connected\":%s,"
             "\"sensor_type\":\"%s\","
             "\"modbus_enabled\":%s,"
             "\"ota_enabled\":%s,"
             "\"ota_state\":\"%s\","
             "\"rule_count\":%d,"
             "\"anomaly_enabled\":%s,"
             "\"anomaly_count\":%d"
             "}",
             g_cfg ? g_cfg->client_id : "unknown",
             EMBMQTTNODE_VERSION,
             (long long)uptime_s,
             mqtt_is_connected() ? "true" : "false",
             g_cfg ? g_cfg->sensor_type : "?",
             (g_cfg && g_cfg->modbus.enabled) ? "true" : "false",
             (g_cfg && g_cfg->ota.enabled) ? "true" : "false",
             ota_state_string(),
             g_cfg ? g_cfg->rule_count : 0,
             (g_cfg && g_cfg->anomaly_enabled) ? "true" : "false",
             g_cfg ? g_cfg->anomaly_count : 0);

    http_send_json(fd, 200, buf);
}

/* GET /api/data/latest */
static void handle_api_data_latest(int fd)
{
    struct sensor_data d;
    int ok;

    pthread_mutex_lock(&g_mutex);
    ok = ring_get_latest(&d);
    pthread_mutex_unlock(&g_mutex);

    if (!ok) {
        http_send_error(fd, 404, "no data yet");
        return;
    }

    char buf[320];
    snprintf(buf, sizeof(buf),
             "{"
             "\"temperature\":%.2f,"
             "\"humidity\":%.2f,"
             "\"pressure\":%.2f,"
             "\"timestamp_ms\":%lld"
             "}",
             d.temperature, d.humidity, d.pressure,
             (long long)d.timestamp_ms);

    http_send_json(fd, 200, buf);
}

/* GET /api/data/history?n=100 */
static void handle_api_data_history(int fd, const char *path)
{
    int n = parse_query_int(path, "n", 60);
    if (n > RING_BUF_SIZE) n = RING_BUF_SIZE;
    if (n < 1) n = 1;

    struct sensor_data records[RING_BUF_SIZE];

    pthread_mutex_lock(&g_mutex);
    int count = ring_get_recent(records, n);
    pthread_mutex_unlock(&g_mutex);

    /* 手动构造 JSON 数组 */
    char *buf = malloc(count * 256 + 32);
    if (!buf) {
        http_send_error(fd, 500, "malloc failed");
        return;
    }

    int pos = 0;
    pos += snprintf(buf + pos, 4, "[\n");
    for (int i = 0; i < count; i++) {
        pos += snprintf(buf + pos, 256,
                        "  {\"temperature\":%.2f,"
                        "\"humidity\":%.2f,"
                        "\"pressure\":%.2f,"
                        "\"timestamp_ms\":%lld}%s\n",
                        records[i].temperature,
                        records[i].humidity,
                        records[i].pressure,
                        (long long)records[i].timestamp_ms,
                        (i < count - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, 4, "]");

    http_send_json(fd, 200, buf);
    free(buf);
}

/* GET /api/rules */
static void handle_api_rules(int fd)
{
    struct rule_stats stats[RULE_MAX];
    int n = rule_engine_get_stats(stats, RULE_MAX);

    if (n <= 0) {
        http_send_json(fd, 200, "[]");
        return;
    }

    /* 手动构造 JSON 数组 */
    char *buf = malloc(n * 256 + 32);
    if (!buf) {
        http_send_error(fd, 500, "malloc failed");
        return;
    }

    int pos = 0;
    pos += snprintf(buf + pos, 4, "[\n");
    for (int i = 0; i < n; i++) {
        pos += snprintf(buf + pos, 256,
                        "  {\"name\":\"%s\","
                        "\"trigger_count\":%d,"
                        "\"last_triggered_ms\":%lld}%s\n",
                        stats[i].name,
                        stats[i].trigger_count,
                        (long long)stats[i].last_triggered,
                        (i < n - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, 4, "]");

    http_send_json(fd, 200, buf);
    free(buf);
}

/* GET /api/anomaly */
static void handle_api_anomaly(int fd)
{
    struct anomaly_stats stats[ANOMALY_MAX];
    int n = anomaly_engine_get_stats(stats, ANOMALY_MAX);

    if (n <= 0) {
        http_send_json(fd, 200, "[]");
        return;
    }

    char *buf = malloc(n * 320 + 32);
    if (!buf) {
        http_send_error(fd, 500, "malloc failed");
        return;
    }

    int pos = 0;
    pos += snprintf(buf + pos, 4, "[\n");
    for (int i = 0; i < n; i++) {
        pos += snprintf(buf + pos, 320,
                        "  {\"name\":\"%s\","
                        "\"trigger_count\":%d,"
                        "\"last_triggered_ms\":%lld,"
                        "\"zscore\":%.4f,"
                        "\"score\":%.4f}%s\n",
                        stats[i].name,
                        stats[i].trigger_count,
                        (long long)stats[i].last_triggered,
                        stats[i].current_zscore,
                        stats[i].current_score,
                        (i < n - 1) ? "," : "");
    }
    pos += snprintf(buf + pos, 4, "]");

    http_send_json(fd, 200, buf);
    free(buf);
}

/* GET /api/ota/status */
static void handle_api_ota_status(int fd)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{"
             "\"ota_enabled\":%s,"
             "\"ota_state\":\"%s\","
             "\"version\":\"%s\""
             "}",
             (g_cfg && g_cfg->ota.enabled) ? "true" : "false",
             ota_state_string(),
             EMBMQTTNODE_VERSION);

    http_send_json(fd, 200, buf);
}

/* POST /api/reboot */
static void handle_api_reboot(int fd, int content_length)
{
    char buf[128];

    /* 读取 body */
    char body[256] = {0};
    if (content_length > 0)
        http_read_body(fd, body, sizeof(body), content_length);

    /* 简单 token 认证 */
    if (strstr(body, "\"token\":\"reboot123\"")) {
        snprintf(buf, sizeof(buf),
                 "{\"status\":\"ok\",\"message\":\"rebooting...\"}");
        http_send_json(fd, 200, buf);

        /* 给客户端一点时间接收响应 */
        usleep(500000);
        LOG_INFO("reboot requested via dashboard");
        /* system("reboot") 在开发环境中仅打印日志或 exit */
#ifdef __linux__
        sync();
        if (system("reboot")) { /* intentionally empty */ }
#else
        LOG_INFO("reboot: would reboot now (not on linux)");
#endif
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"error\":\"unauthorized: invalid or missing token\"}");
        http_send_json(fd, 403, buf);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * 内嵌 HTML 仪表盘
 * ═══════════════════════════════════════════════════════════════ */

static const char DASHBOARD_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>EmbMQTTNode Dashboard</title>\n"
"<style>\n"
"* { margin:0; padding:0; box-sizing:border-box; }\n"
"body {\n"
"  font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;\n"
"  background: #0f1923; color: #c8d6e5; min-height:100vh;\n"
"}\n"
".header {\n"
"  background: linear-gradient(135deg, #1a2a3a 0%, #0d1b2a 100%);\n"
"  border-bottom: 2px solid #2c3e50;\n"
"  padding: 12px 24px; display:flex; justify-content:space-between;\n"
"  align-items:center; flex-wrap:wrap; gap:8px;\n"
"}\n"
".header h1 { font-size:1.3em; color:#45aaf2; font-weight:600; }\n"
".header .meta { font-size:0.8em; color:#778ca3; }\n"
".badge { display:inline-block; padding:3px 10px; border-radius:10px;\n"
"  font-size:0.75em; font-weight:600; }\n"
".badge-ok  { background:#20bf6b20; color:#20bf6b; }\n"
".badge-warn{ background:#f7b73120; color:#f7b731; }\n"
".badge-err { background:#fc5c6520; color:#fc5c65; }\n"
".badge-off { background:#778ca330; color:#778ca3; }\n"
".grid {\n"
"  display:grid; grid-template-columns:repeat(auto-fit,minmax(280px,1fr));\n"
"  gap:16px; padding:16px; max-width:1400px; margin:0 auto;\n"
"}\n"
".card {\n"
"  background:#1a2a3a; border:1px solid #2c3e50; border-radius:8px;\n"
"  padding:16px; box-shadow:0 2px 8px rgba(0,0,0,.25);\n"
"}\n"
".card h2 { font-size:0.9em; color:#778ca3; text-transform:uppercase;\n"
"  letter-spacing:1px; margin-bottom:12px; border-bottom:1px solid #2c3e50;\n"
"  padding-bottom:8px; }\n"
".value-big { font-size:2.4em; font-weight:700; color:#ecf0f1; }\n"
".value-unit { font-size:0.5em; color:#778ca3; margin-left:4px; }\n"
".value-row { display:flex; justify-content:space-between; padding:4px 0;\n"
"  border-bottom:1px solid #1e3244; font-size:0.9em; }\n"
".value-row .label { color:#778ca3; }\n"
".value-row .val { color:#c8d6e5; font-weight:500; }\n"
".full-width { grid-column:1 / -1; }\n"
"table { width:100%; border-collapse:collapse; font-size:0.85em; }\n"
"th { text-align:left; color:#778ca3; padding:6px 8px;\n"
"  border-bottom:2px solid #2c3e50; }\n"
"td { padding:5px 8px; border-bottom:1px solid #1e3244; }\n"
".chart-bar { display:flex; gap:2px; align-items:flex-end; height:80px;\n"
"  padding:4px 0; }\n"
".chart-bar div { flex:1; min-width:3px; background:#45aaf2;\n"
"  border-radius:2px 2px 0 0; opacity:0.7; transition:opacity .2s; }\n"
".chart-bar div:hover { opacity:1; }\n"
".refresh { font-size:0.7em; color:#778ca3; }\n"
".alert-tag { display:inline-block; padding:2px 8px; border-radius:4px;\n"
"  font-size:0.75em; margin:2px; }\n"
".alert-tag.active { background:#fc5c6520; color:#fc5c65; }\n"
".alert-tag.idle   { background:#20bf6b20; color:#20bf6b; }\n"
"@media (max-width:600px) {\n"
"  .grid { grid-template-columns:1fr; padding:8px; }\n"
"  .header h1 { font-size:1.1em; }\n"
"  .value-big { font-size:1.8em; }\n"
"}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"header\">\n"
"  <div>\n"
"    <h1>⚙ EmbMQTTNode Gateway</h1>\n"
"    <span class=\"meta\">\n"
"      版本 <span id=\"ver\">--</span> &nbsp;\n"
"      设备 <span id=\"devid\">--</span> &nbsp;\n"
"      运行时间 <span id=\"uptime\">--</span>\n"
"    </span>\n"
"  </div>\n"
"  <div style=\"display:flex;gap:8px;flex-wrap:wrap;\">\n"
"    <span id=\"mqtt\" class=\"badge badge-off\">MQTT</span>\n"
"    <span id=\"modbus\" class=\"badge badge-off\">Modbus</span>\n"
"    <span id=\"ota\" class=\"badge badge-off\">OTA</span>\n"
"    <span id=\"sensor\" class=\"badge badge-off\">Sensor</span>\n"
"  </div>\n"
"</div>\n"
"\n"
"<div class=\"grid\">\n"
"  <!-- 温度 -->\n"
"  <div class=\"card\">\n"
"    <h2>🌡 温度</h2>\n"
"    <div class=\"value-big\" id=\"temp\">--<span class=\"value-unit\">°C</span></div>\n"
"  </div>\n"
"  <!-- 湿度 -->\n"
"  <div class=\"card\">\n"
"    <h2>💧 湿度</h2>\n"
"    <div class=\"value-big\" id=\"hum\">--<span class=\"value-unit\">%RH</span></div>\n"
"  </div>\n"
"  <!-- 气压 -->\n"
"  <div class=\"card\">\n"
"    <h2>📊 气压</h2>\n"
"    <div class=\"value-big\" id=\"pres\">--<span class=\"value-unit\">hPa</span></div>\n"
"  </div>\n"
"\n"
"  <!-- 系统状态 -->\n"
"  <div class=\"card\">\n"
"    <h2>📋 系统状态</h2>\n"
"    <div class=\"value-row\"><span class=\"label\">传感器类型</span><span class=\"val\" id=\"stype\">--</span></div>\n"
"    <div class=\"value-row\"><span class=\"label\">规则数量</span><span class=\"val\" id=\"rcount\">--</span></div>\n"
"    <div class=\"value-row\"><span class=\"label\">OTA 状态</span><span class=\"val\" id=\"ostate\">--</span></div>\n"
"    <div class=\"value-row\"><span class=\"label\">最后更新</span><span class=\"val\" id=\"updated\">--</span></div>\n"
"  </div>\n"
"\n"
"  <!-- 异常检测引擎 -->\n"
"  <div class=\"card\">\n"
"    <h2>⚠ 异常检测</h2>\n"
"    <div id=\"anomalies\"><span style=\"color:#778ca3\">未启用或未配置</span></div>\n"
"  </div>\n"
"\n"
"  <!-- 规则引擎 -->\n"
"  <div class=\"card\">\n"
"    <h2>🔔 规则引擎</h2>\n"
"    <div id=\"rules\"><span style=\"color:#778ca3\">无规则配置</span></div>\n"
"  </div>\n"
"\n"
"  <!-- 最近数据趋势 -->\n"
"  <div class=\"card full-width\">\n"
"    <h2>📈 温度趋势（最近 60 条）<span class=\"refresh\" id=\"trend_label\"></span></h2>\n"
"    <div class=\"chart-bar\" id=\"chart\"></div>\n"
"  </div>\n"
"</div>\n"
"\n"
"<script>\n"
"// ── 工具 ────────────────────────────────────────────\n"
"function $(id) { return document.getElementById(id); }\n"
"\n"
"// ── 格式化 ──────────────────────────────────────────\n"
"function fmtTime(ts) {\n"
"  if (!ts) return '--';\n"
"  var d = new Date(ts);\n"
"  return d.toLocaleTimeString();\n"
"}\n"
"function fmtUptime(s) {\n"
"  if (!s && s !== 0) return '--';\n"
"  s = parseInt(s);\n"
"  var h = Math.floor(s/3600), m = Math.floor((s%3600)/60), sec = s%60;\n"
"  return h+'h '+m+'m '+sec+'s';\n"
"}\n"
"\n"
"// ── 轮询 /api/status ────────────────────────────────\n"
"async function pollStatus() {\n"
"  try {\n"
"    var r = await fetch('/api/status');\n"
"    var d = await r.json();\n"
"    $('ver').textContent = d.version;\n"
"    $('devid').textContent = d.client_id;\n"
"    $('uptime').textContent = fmtUptime(d.uptime_seconds);\n"
"    $('stype').textContent = d.sensor_type;\n"
"    $('rcount').textContent = d.rule_count;\n"
"    $('ostate').textContent = d.ota_state;\n"
"    // badges\n"
"    setBadge('mqtt',  d.mqtt_connected);\n"
"    setBadge('modbus',d.modbus_enabled);\n"
"    setBadge('ota',   d.ota_enabled);\n"
"    $('sensor').textContent = d.sensor_type;\n"
"    $('sensor').className = 'badge badge-ok';\n"
"  } catch(e) { console.warn('status poll:', e); }\n"
"}\n"
"function setBadge(id, on) {\n"
"  var el = $(id);\n"
"  if (on) { el.textContent = id.toUpperCase()+' ON'; el.className='badge badge-ok'; }\n"
"  else    { el.textContent = id.toUpperCase()+' OFF'; el.className='badge badge-off'; }\n"
"}\n"
"\n"
"// ── 轮询 /api/data/latest ───────────────────────────\n"
"async function pollLatest() {\n"
"  try {\n"
"    var r = await fetch('/api/data/latest');\n"
"    if (!r.ok) return;\n"
"    var d = await r.json();\n"
"    $('temp').innerHTML = d.temperature.toFixed(1) + '<span class=\"value-unit\">°C</span>';\n"
"    $('hum').innerHTML  = d.humidity.toFixed(1) + '<span class=\"value-unit\">%RH</span>';\n"
"    $('pres').innerHTML = d.pressure.toFixed(1) + '<span class=\"value-unit\">hPa</span>';\n"
"    $('updated').textContent = fmtTime(d.timestamp_ms);\n"
"  } catch(e) { /* silent */ }\n"
"}\n"
"\n"
"// ── 轮询 /api/rules ─────────────────────────────────\n"
"async function pollRules() {\n"
"  try {\n"
"    var r = await fetch('/api/rules');\n"
"    var arr = await r.json();\n"
"    if (!arr || arr.length === 0) {\n"
"      $('rules').innerHTML = '<span style=\"color:#778ca3\">无规则或未配置</span>';\n"
"      return;\n"
"    }\n"
"    var html = '<table><tr><th>规则</th><th>触发次数</th><th>最后触发</th><th>状态</th></tr>';\n"
"    arr.forEach(function(rule) {\n"
"      var active = rule.trigger_count > 0 ? 'active':'idle';\n"
"      var label  = rule.trigger_count > 0 ? '活跃':'空闲';\n"
"      html += '<tr><td>'+escHtml(rule.name)+'</td><td>'+rule.trigger_count+'</td><td>'\n"
"           + fmtTime(rule.last_triggered_ms)+'</td>'\n"
"           + '<td><span class=\"alert-tag '+active+'\">'+label+'</span></td></tr>';\n"
"    });\n"
"    html += '</table>';\n"
"    $('rules').innerHTML = html;\n"
"  } catch(e) { /* silent */ }\n"
"}\n"
"function escHtml(s) { var d=document.createElement('div'); d.textContent=s; return d.innerHTML; }\n"
"\n"
"// ── 轮询 /api/data/history ── 温度趋势图 ────────────\n"
"async function pollTrend() {\n"
"  try {\n"
"    var r = await fetch('/api/data/history?n=60');\n"
"    var arr = await r.json();\n"
"    if (!arr || arr.length === 0) return;\n"
"    var temps = arr.map(function(d){ return d.temperature; });\n"
"    var tmin = Math.min.apply(null, temps);\n"
"    var tmax = Math.max.apply(null, temps);\n"
"    var range = (tmax - tmin) || 1;\n"
"    var html = '';\n"
"    temps.forEach(function(t){\n"
"      var h = ((t - tmin) / range) * 100;\n"
"      html += '<div style=\"height:'+Math.max(h,2)+'%\" title=\"'+t.toFixed(1)+'°C\"></div>';\n"
"    });\n"
"    $('chart').innerHTML = html;\n"
"    $('trend_label').textContent = '范围: '+tmin.toFixed(1)+' ~ '+tmax.toFixed(1)+' °C';\n"
"  } catch(e) { /* silent */ }\n"
"}\n"
"\n"
"// ── 轮询 /api/anomaly ───────────────────────────────\n"
"async function pollAnomalies() {\n"
"  try {\n"
"    var r = await fetch('/api/anomaly');\n"
"    var arr = await r.json();\n"
"    if (!arr || arr.length === 0) {\n"
"      $('anomalies').innerHTML = '<span style=\"color:#778ca3\">无异常规则或未触发</span>';\n"
"      return;\n"
"    }\n"
"    var html = '<table><tr><th>规则</th><th>触发</th><th>Z-Score</th><th>分数</th><th>状态</th></tr>';\n"
"    arr.forEach(function(a) {\n"
"      var active = a.trigger_count > 0 ? 'active':'idle';\n"
"      var label  = a.trigger_count > 0 ? '⚠ 异常':'✓ 正常';\n"
"      html += '<tr><td>'+escHtml(a.name)+'</td><td>'+a.trigger_count+'</td>'\n"
"           + '<td>'+a.zscore.toFixed(2)+'</td><td>'+a.score.toFixed(3)+'</td>'\n"
"           + '<td><span class=\"alert-tag '+active+'\">'+label+'</span></td></tr>';\n"
"    });\n"
"    html += '</table>';\n"
"    $('anomalies').innerHTML = html;\n"
"  } catch(e) {}\n"
"}\n"
"\n"
"// ── 定时轮询 ─────────────────────────────────────────\n"
"pollStatus(); pollLatest(); pollAnomalies(); pollRules(); pollTrend();\n"
"setInterval(pollStatus, 5000);\n"
"setInterval(pollLatest, 2000);\n"
"setInterval(pollAnomalies, 5000);\n"
"setInterval(pollRules, 5000);\n"
"setInterval(pollTrend, 10000);\n"
"</script>\n"
"</body>\n"
"</html>\n";

/* ═══════════════════════════════════════════════════════════════
 * 请求路由分发
 * ═══════════════════════════════════════════════════════════════ */

static void http_dispatch(int fd, const char *method, const char *path,
                         int content_length)
{
    /* GET / */
    if (strcmp(method, "GET") == 0 &&
        (strcmp(path, "/") == 0 || strncmp(path, "/index", 6) == 0)) {
        http_send(fd, 200, "text/html; charset=utf-8",
                  DASHBOARD_HTML, (int)strlen(DASHBOARD_HTML));
        return;
    }

    /* GET /api/status */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        handle_api_status(fd);
        return;
    }

    /* GET /api/data/latest */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/data/latest") == 0) {
        handle_api_data_latest(fd);
        return;
    }

    /* GET /api/data/history?n=N */
    if (strcmp(method, "GET") == 0 &&
        strncmp(path, "/api/data/history", 17) == 0) {
        handle_api_data_history(fd, path);
        return;
    }

    /* GET /api/rules */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/rules") == 0) {
        handle_api_rules(fd);
        return;
    }

    /* GET /api/anomaly */
    if (strcmp(method, "GET") == 0 &&
        strcmp(path, "/api/anomaly") == 0) {
        handle_api_anomaly(fd);
        return;
    }

    /* GET /api/ota/status */
    if (strcmp(method, "GET") == 0 &&
        strcmp(path, "/api/ota/status") == 0) {
        handle_api_ota_status(fd);
        return;
    }

    /* POST /api/reboot */
    if (strcmp(method, "POST") == 0 &&
        strcmp(path, "/api/reboot") == 0) {
        handle_api_reboot(fd, content_length);
        return;
    }

    /* 404 */
    http_send_error(fd, 404, "not found");
}

/* ═══════════════════════════════════════════════════════════════
 * HTTP 连接处理（每连接一个周期：接收 → 解析 → 路由 → 关闭）
 * ═══════════════════════════════════════════════════════════════ */

static void http_handle_client(int fd)
{
    char line[HTTP_MAX_PATH + 64];
    int n = http_read_line(fd, line, sizeof(line));
    if (n <= 0) goto done;

    char method[16] = {0};
    char path[HTTP_MAX_PATH] = {0};

    if (http_parse_request_line(line, method, sizeof(method),
                                path, sizeof(path)) != 0) {
        http_send_error(fd, 400, "bad request");
        goto done;
    }

    /* 读取请求头并提取 Content-Length */
    int content_length = http_read_headers(fd);

    /* 分发 */
    http_dispatch(fd, method, path, content_length);

done:
    close(fd);
}

/* ═══════════════════════════════════════════════════════════════
 * 公开 API
 * ═══════════════════════════════════════════════════════════════ */

int http_server_start(int port, const char *bind_addr,
                      const struct device_info *dev,
                      const struct node_config *cfg)
{
    if (port <= 0) port = HTTP_PORT_DEFAULT;

    /* 保存只读引用 */
    g_dev = dev;
    g_cfg = cfg;
    g_start_time_ms = time(NULL) * 1000LL;

    /* 创建 socket */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        LOG_ERROR("http_server: socket() failed: %s", strerror(errno));
        return E_IO;
    }

    /* 端口快速重用 */
    int reuse = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR,
               &reuse, sizeof(reuse));

    /* bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (bind_addr && bind_addr[0]) {
        inet_pton(AF_INET, bind_addr, &addr.sin_addr);
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("http_server: bind(:%d) failed: %s", port, strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return E_IO;
    }

    if (listen(g_listen_fd, HTTP_BACKLOG) < 0) {
        LOG_ERROR("http_server: listen() failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return E_IO;
    }

    g_running = 1;
    LOG_INFO("http dashboard started on http://0.0.0.0:%d", port);

    /* accept 循环 */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_listen_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;   /* 信号中断，检查 g_running */
            if (!g_running)
                break;
            LOG_ERROR("http_server: accept() failed: %s", strerror(errno));
            continue;
        }

        http_handle_client(client_fd);
    }

    close(g_listen_fd);
    g_listen_fd = -1;
    LOG_INFO("http dashboard stopped");
    return E_OK;
}

void http_server_stop(void)
{
    g_running = 0;
    /* 触发 accept 退出：发送信号或 shutdown */
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
    }
}

void http_server_update_data(const struct sensor_data *data)
{
    if (!data) return;
    pthread_mutex_lock(&g_mutex);
    ring_push(data);
    pthread_mutex_unlock(&g_mutex);
}
