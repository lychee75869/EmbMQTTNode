/*
 * ota.c
 * A/B 分区 OTA 远程升级实现（阶段四）
 *
 * ── 升级流程 ──
 * 1. MQTT 收到 upgrade 指令（JSON）
 * 2. HTTP GET 下载固件到 download/ 目录
 * 3. SHA256 校验固件完整性
 * 4. 解压/复制到备用槽位
 * 5. 更新 current_slot 文件，切换到新槽位
 * 6. 退出程序，由 systemd 自动重启
 * 7. 启动后 post_boot_check：成功→上报 running，失败→回滚
 *
 * ── 安全 ──
 * - SHA256 固件完整性校验（需 libcrypto）
 * - 启动失败自动回滚（boot attempt 计数 ≤ max_attempts）
 * - A/B 双槽隔离，升级失败不影响当前运行版本
 */

#include "ota.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdarg.h>
#include <strings.h>
#include <openssl/evp.h>

/* ─── 内部常量 ─────────────────────────────────────────── */

#define OTA_STATUS_TOPIC_FMT  "embmqttnode/%s/ota/status"
#define OTA_CMD_TOPIC_FMT     "embmqttnode/%s/ota/cmd"
#define OTA_DOWNLOAD_SUBDIR   "download"
#define OTA_FIRMWARE_FILE     "firmware.bin"
#define OTA_CURRENT_SLOT_FILE "current_slot"
#define OTA_BOOT_COUNT_FILE   "boot_count"
#define OTA_HTTP_BUF_SIZE     4096
#define OTA_JSON_BUF_SIZE     2048

/* ─── 内部状态 ─────────────────────────────────────────── */

static struct ota_config g_cfg;
static char              g_client_id[64];
static char              g_current_version[OTA_VERSION_MAX];
static char              g_slot_dir[256];
static char              g_status_topic[256];

/* OTA 上下文（运行时状态，持久化部分写入文件） */
static enum ota_state g_state         = OTA_STATE_IDLE;
static int            g_active_slot   = 0;          /* 0=A, 1=B */
static int            g_target_slot   = -1;
static int            g_download_pct  = 0;
static int            g_boot_attempt  = 0;
static char           g_target_version[OTA_VERSION_MAX];
static char           g_download_url[OTA_URL_MAX];
static char           g_expected_checksum[OTA_CHECKSUM_MAX];

/* MQTT 发布回调（由 main 注入） */
static int (*g_mqtt_publish)(const char *topic,
                              const char *payload, int qos) = NULL;

/* ── 辅助函数声明 ───────────────────────────────────────── */

static void ota_report_status(const char *state, const char *extra_fmt, ...);
static int  ota_read_slot_file(const char *filename, char *buf, int buflen);
static int  ota_write_slot_file(const char *filename, const char *content);
static int  ota_ensure_dirs(void);
static int  ota_http_download(const char *url, const char *dest_path);
static int  ota_sha256_file(const char *path, char *hash_out, int hash_len);
static int  ota_install_firmware(const char *src_path, int target_slot);
static int  ota_switch_slot(int slot);
static int  ota_parse_upgrade_cmd(const char *json, int len);

/* ═══════════════════════════════════════════════════════════
 * 公开 API
 * ═══════════════════════════════════════════════════════════ */

int ota_init(const struct ota_config *cfg,
             const char *client_id,
             const char *current_version)
{
    if (!cfg || !client_id || !current_version)
        return E_INVAL;

    memcpy(&g_cfg, cfg, sizeof(*cfg));

    strncpy(g_client_id, client_id, sizeof(g_client_id) - 1);
    g_client_id[sizeof(g_client_id) - 1] = '\0';

    strncpy(g_current_version, current_version, sizeof(g_current_version) - 1);
    g_current_version[sizeof(g_current_version) - 1] = '\0';

    /* 槽位根目录 */
    if (g_cfg.slot_dir[0] != '\0')
        snprintf(g_slot_dir, sizeof(g_slot_dir), "%s", g_cfg.slot_dir);
    else
        snprintf(g_slot_dir, sizeof(g_slot_dir), "%s",
                 OTA_SLOT_DIR_DEFAULT);

    /* MQTT 状态 topic */
    snprintf(g_status_topic, sizeof(g_status_topic),
             OTA_STATUS_TOPIC_FMT, g_client_id);

    /* 确保目录结构存在 */
    ota_ensure_dirs();

    /* 读取当前活动槽位 */
    char slot_char[2] = {0};
    if (ota_read_slot_file(OTA_CURRENT_SLOT_FILE, slot_char, sizeof(slot_char)) > 0) {
        g_active_slot = (slot_char[0] == 'B') ? 1 : 0;
    } else {
        /* 首次启动，默认 slot A */
        ota_write_slot_file(OTA_CURRENT_SLOT_FILE, "A");
        g_active_slot = 0;
    }

    /* 读取启动尝试计数 */
    char count_buf[16] = {0};
    if (ota_read_slot_file(OTA_BOOT_COUNT_FILE, count_buf, sizeof(count_buf)) > 0) {
        g_boot_attempt = atoi(count_buf);
    }

    g_state = OTA_STATE_IDLE;

    LOG_INFO("ota init ok: slot=%c dir=%s version=%s boot_attempt=%d",
             g_active_slot ? 'B' : 'A', g_slot_dir,
             g_current_version, g_boot_attempt);

    if (g_cfg.enabled) {
        LOG_INFO("ota: enabled, boot_attempt_max=%d", g_cfg.boot_attempt_max);
    } else {
        LOG_INFO("ota: disabled by config, OTA commands will be ignored");
    }

    return E_OK;
}

void ota_set_mqtt_publish(int (*publish_cb)(const char *topic,
                                             const char *payload,
                                             int qos))
{
    g_mqtt_publish = publish_cb;
}

void ota_handle_message(const char *payload, int payload_len)
{
    if (!g_cfg.enabled) {
        LOG_INFO("ota: ignored (disabled)");
        return;
    }
    if (g_state != OTA_STATE_IDLE) {
        LOG_WARN("ota: busy (state=%s), ignoring command",
                 ota_state_string());
        return;
    }

    if (ota_parse_upgrade_cmd(payload, payload_len) != E_OK) {
        ota_report_status("error", "\"Invalid OTA command JSON\"");
        return;
    }

    LOG_INFO("ota: upgrade cmd received version=%s", g_target_version);
    g_state = OTA_STATE_DOWNLOADING;
}

int ota_check_and_handle(void)
{
    if (!g_cfg.enabled)
        return 0;
    if (g_state == OTA_STATE_IDLE)
        return 0;

    switch (g_state) {

    case OTA_STATE_DOWNLOADING: {
        char dest_path[512];
        snprintf(dest_path, sizeof(dest_path), "%s/%s/%s",
                 g_slot_dir, OTA_DOWNLOAD_SUBDIR, OTA_FIRMWARE_FILE);

        ota_report_status("downloading", NULL);
        LOG_INFO("ota: downloading %s → %s", g_download_url, dest_path);

        if (ota_http_download(g_download_url, dest_path) != E_OK) {
            ota_report_status("error", "\"HTTP download failed\"");
            g_state = OTA_STATE_FAILED;
            break;
        }
        g_download_pct = 100;
        g_state = OTA_STATE_VERIFYING;
        break;
    }

    case OTA_STATE_VERIFYING: {
        char hash_str[128] = {0};
        char src_path[512];
        snprintf(src_path, sizeof(src_path), "%s/%s/%s",
                 g_slot_dir, OTA_DOWNLOAD_SUBDIR, OTA_FIRMWARE_FILE);

        ota_report_status("verifying", NULL);

        if (ota_sha256_file(src_path, hash_str, sizeof(hash_str)) != E_OK) {
            ota_report_status("error", "\"SHA256 computation failed\"");
            g_state = OTA_STATE_FAILED;
            break;
        }

        LOG_INFO("ota: sha256 computed = %s", hash_str);
        LOG_INFO("ota: sha256 expected = %s", g_expected_checksum);

        /* 比较校验和（兼容带 "sha256:" 前缀的格式） */
        const char *exp = g_expected_checksum;
        if (strncmp(exp, "sha256:", 7) == 0)
            exp += 7;

        if (strcasecmp(hash_str, exp) != 0) {
            LOG_ERROR("ota: checksum mismatch!");
            ota_report_status("error", "\"Checksum mismatch\"");
            g_state = OTA_STATE_FAILED;
            break;
        }

        ota_report_status("verifying", "\"checksum_ok\":true");
        g_state = OTA_STATE_INSTALLING;
        break;
    }

    case OTA_STATE_INSTALLING: {
        /* 目标槽位 = 非当前槽 */
        g_target_slot = (g_active_slot == 0) ? 1 : 0;

        char src_path[512];
        snprintf(src_path, sizeof(src_path), "%s/%s/%s",
                 g_slot_dir, OTA_DOWNLOAD_SUBDIR, OTA_FIRMWARE_FILE);

        ota_report_status("installing",
                          "\"target_slot\":\"%c\"",
                          g_target_slot ? 'B' : 'A');

        if (ota_install_firmware(src_path, g_target_slot) != E_OK) {
            ota_report_status("error", "\"Installation failed\"");
            g_state = OTA_STATE_FAILED;
            break;
        }

        /* 重置启动计数（新版本从零开始） */
        ota_write_slot_file(OTA_BOOT_COUNT_FILE, "0");

        /* 切换活动槽位 */
        if (ota_switch_slot(g_target_slot) != E_OK) {
            ota_report_status("error", "\"Slot switch failed\"");
            g_state = OTA_STATE_FAILED;
            break;
        }

        g_state = OTA_STATE_REBOOTING;
        break;
    }

    case OTA_STATE_REBOOTING: {
        ota_report_status("rebooting", NULL);
        LOG_INFO("ota: rebooting to slot %c...",
                 g_target_slot ? 'B' : 'A');

        /* 给 MQTT 一点时间发送状态消息 */
        usleep(500000);

        /* 优雅退出 → 由 systemd Restart=always 自动重启 */
        LOG_INFO("ota: exiting for reboot (expect systemd to restart)");
        exit(0);
        break;
    }

    case OTA_STATE_FAILED:
        LOG_WARN("ota: in failed state, resetting to IDLE");
        g_state = OTA_STATE_IDLE;
        break;

    default:
        break;
    }

    return (g_state != OTA_STATE_IDLE) ? 1 : 0;
}

void ota_post_boot_check(void)
{
    if (!g_cfg.enabled)
        return;

    int max_attempts = g_cfg.boot_attempt_max;
    if (max_attempts <= 0)
        max_attempts = OTA_BOOT_ATTEMPT_MAX;

    /* 读取当前计数 */
    char count_buf[16] = {0};
    if (ota_read_slot_file(OTA_BOOT_COUNT_FILE, count_buf, sizeof(count_buf)) > 0) {
        g_boot_attempt = atoi(count_buf);
    } else {
        g_boot_attempt = 0;
    }

    /* 首次启动（计数为 0）：正常启动，无需额外处理 */
    if (g_boot_attempt == 0) {
        LOG_INFO("ota: fresh boot on slot %c, no rollback needed",
                 g_active_slot ? 'B' : 'A');
        ota_report_status("running", "\"slot\":\"%c\"",
                          g_active_slot ? 'B' : 'A');
        return;
    }

    /* 启动计数 > 0：说明上次启动后崩溃了 */
    LOG_WARN("ota: boot attempt %d/%d on slot %c",
             g_boot_attempt, max_attempts,
             g_active_slot ? 'B' : 'A');

    if (g_boot_attempt >= max_attempts) {
        /* 达到最大尝试次数 → 回滚 */
        LOG_ERROR("ota: max boot attempts reached, rolling back!");
        ota_report_status("rollback",
                          "\"reason\":\"boot_failed_%d_times\"",
                          g_boot_attempt);

        /* 切回备用槽位（旧版本） */
        int fallback_slot = (g_active_slot == 0) ? 1 : 0;
        ota_switch_slot(fallback_slot);
        ota_write_slot_file(OTA_BOOT_COUNT_FILE, "0");

        LOG_INFO("ota: rolled back to slot %c, rebooting...",
                 fallback_slot ? 'B' : 'A');
        usleep(500000);
        exit(0);
    } else {
        /* 还没达到上限 → 递增计数，尝试继续运行 */
        g_boot_attempt++;
        char new_count[16];
        snprintf(new_count, sizeof(new_count), "%d", g_boot_attempt);
        ota_write_slot_file(OTA_BOOT_COUNT_FILE, new_count);

        LOG_WARN("ota: incrementing boot attempt to %d/%d",
                 g_boot_attempt, max_attempts);

        /* 如果这次能稳定运行，后续会重置计数器 */
        ota_report_status("running",
                          "\"slot\":\"%c\",\"boot_attempt\":%d",
                          g_active_slot ? 'B' : 'A', g_boot_attempt);
    }
}

const char *ota_state_string(void)
{
    switch (g_state) {
    case OTA_STATE_IDLE:        return "idle";
    case OTA_STATE_DOWNLOADING: return "downloading";
    case OTA_STATE_VERIFYING:   return "verifying";
    case OTA_STATE_INSTALLING:  return "installing";
    case OTA_STATE_REBOOTING:   return "rebooting";
    case OTA_STATE_FAILED:      return "failed";
    default:                    return "unknown";
    }
}

void ota_close(void)
{
    g_mqtt_publish = NULL;
    g_state = OTA_STATE_IDLE;
    LOG_INFO("ota closed");
}

/* ═══════════════════════════════════════════════════════════
 * 内部函数
 * ═══════════════════════════════════════════════════════════ */

/* ─── MQTT 状态上报 ─────────────────────────────────────── */

static void ota_report_status(const char *state, const char *extra_fmt, ...)
{
    if (!g_mqtt_publish)
        return;

    char payload[512];
    int off = snprintf(payload, sizeof(payload),
                       "{\"state\":\"%s\",\"version\":\"%s\"",
                       state, g_target_version[0] ? g_target_version
                                                  : g_current_version);

    if (g_download_pct > 0 && g_download_pct < 100) {
        off += snprintf(payload + off, sizeof(payload) - off,
                        ",\"progress\":%d", g_download_pct);
    }

    if (extra_fmt) {
        off += snprintf(payload + off, sizeof(payload) - off, ",");
        va_list ap;
        va_start(ap, extra_fmt);
        vsnprintf(payload + off, sizeof(payload) - off, extra_fmt, ap);
        va_end(ap);
    }

    /* 确保 JSON 闭合 */
    size_t len = strlen(payload);
    if (len < sizeof(payload) - 2)
        strcat(payload, "}");

    g_mqtt_publish(g_status_topic, payload, 1);
    LOG_INFO("ota status: %s", payload);
}

/* ─── 槽位文件读写 ─────────────────────────────────────── */

static int ota_read_slot_file(const char *filename, char *buf, int buflen)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_slot_dir, filename);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    if (fgets(buf, buflen, fp)) {
        /* trim trailing newline */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
            buf[len - 1] = '\0';
        fclose(fp);
        return (int)strlen(buf);
    }

    fclose(fp);
    return -1;
}

static int ota_write_slot_file(const char *filename, const char *content)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_slot_dir, filename);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR("ota: write %s failed: %s", path, strerror(errno));
        return E_IO;
    }

    fprintf(fp, "%s\n", content);
    fclose(fp);
    return E_OK;
}

/* ─── 目录初始化 ───────────────────────────────────────── */

static int ota_ensure_dirs(void)
{
    char path[512];

    /* 创建根目录 */
    mkdir(g_slot_dir, 0755);

    /* 创建槽位目录 */
    snprintf(path, sizeof(path), "%s/slot_a", g_slot_dir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/slot_b", g_slot_dir);
    mkdir(path, 0755);

    /* 创建下载目录 */
    snprintf(path, sizeof(path), "%s/%s", g_slot_dir, OTA_DOWNLOAD_SUBDIR);
    mkdir(path, 0755);

    return E_OK;
}

/* ─── HTTP GET 下载 ─────────────────────────────────────── */

static int ota_http_download(const char *url, const char *dest_path)
{
    /* 解析 URL: http://host[:port]/path */
    const char *proto = "http://";
    if (strncmp(url, proto, 7) != 0) {
        LOG_ERROR("ota: only HTTP supported");
        return E_NET;
    }

    const char *host_start = url + 7;
    const char *path_start = strchr(host_start, '/');
    char host[256];
    char path[512];
    int  port = 80;

    if (path_start) {
        size_t host_len = (size_t)(path_start - host_start);
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        memcpy(host, host_start, host_len);
        host[host_len] = '\0';
        strncpy(path, path_start, sizeof(path));
        path[sizeof(path) - 1] = '\0';
    } else {
        size_t host_len = strlen(host_start);
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        memcpy(host, host_start, host_len);
        host[host_len] = '\0';
        strncpy(path, "/", sizeof(path));
    }

    /* 检查 host 中是否包含端口 */
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }

    LOG_INFO("ota: http GET host=%s port=%d path=%s", host, port, path);

    /* DNS 解析 */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        LOG_ERROR("ota: DNS lookup failed for %s", host);
        return E_NET;
    }

    /* 创建 socket 并连接 */
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        LOG_ERROR("ota: socket() failed: %s", strerror(errno));
        freeaddrinfo(res);
        return E_NET;
    }

    /* 连接超时 10s */
    struct timeval tv = {10, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        LOG_ERROR("ota: connect() failed: %s", strerror(errno));
        close(sock);
        freeaddrinfo(res);
        return E_NET;
    }
    freeaddrinfo(res);

    /* 发送 HTTP GET 请求 */
    char request[1024];
    int req_len = snprintf(request, sizeof(request),
                           "GET %s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "User-Agent: EmbMQTTNode-OTA/1.0\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           path, host);

    if (send(sock, request, req_len, 0) < 0) {
        LOG_ERROR("ota: send() failed: %s", strerror(errno));
        close(sock);
        return E_NET;
    }

    /* 接收响应并写入文件 */
    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        LOG_ERROR("ota: fopen %s failed: %s", dest_path, strerror(errno));
        close(sock);
        return E_IO;
    }

    char buf[OTA_HTTP_BUF_SIZE];
    int  header_done = 0;
    int  total_bytes = 0;
    int  status_code = 0;

    while (1) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0)
            break;

        if (!header_done) {
            /* 查找 HTTP 状态码 */
            if (status_code == 0) {
                char *sp = strchr(buf, ' ');
                if (sp) status_code = atoi(sp + 1);
            }

            /* 查找 header 结束标记 \r\n\r\n */
            char *body = strstr(buf, "\r\n\r\n");
            if (body) {
                header_done = 1;
                int header_len = (body - buf) + 4;
                fwrite(body + 4, 1, n - header_len, fp);
                total_bytes += n - header_len;
            }
            continue;
        }

        fwrite(buf, 1, n, fp);
        total_bytes += n;
    }

    fclose(fp);
    close(sock);

    if (status_code != 200) {
        LOG_ERROR("ota: HTTP %d", status_code);
        unlink(dest_path);
        return E_NET;
    }

    LOG_INFO("ota: downloaded %d bytes", total_bytes);
    g_download_pct = 100;
    return (total_bytes > 0) ? E_OK : E_NET;
}

/* ─── SHA256 文件校验 ───────────────────────────────────── */

static int ota_sha256_file(const char *path, char *hash_out, int hash_len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        LOG_ERROR("ota: fopen %s for sha256: %s", path, strerror(errno));
        return E_IO;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        fclose(fp);
        return E_NO_MEM;
    }

    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);

    unsigned char buf[OTA_HTTP_BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        EVP_DigestUpdate(ctx, buf, n);
    }
    fclose(fp);

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  md_len = 0;
    EVP_DigestFinal_ex(ctx, md, &md_len);
    EVP_MD_CTX_free(ctx);

    /* 转为小写 hex 字符串 */
    int off = 0;
    for (unsigned int i = 0; i < md_len && off < hash_len - 1; i++) {
        off += snprintf(hash_out + off, hash_len - off,
                        "%02x", md[i]);
    }
    hash_out[off] = '\0';

    return E_OK;
}

/* ─── 固件安装到槽位 ───────────────────────────────────── */

static int ota_install_firmware(const char *src_path, int target_slot)
{
    char dest_path[512];
    char dest_dir[512];

    snprintf(dest_dir, sizeof(dest_dir), "%s/slot_%c",
             g_slot_dir, target_slot ? 'b' : 'a');
    int dlen = snprintf(dest_path, sizeof(dest_path), "%s/embmqttnode",
                        dest_dir);
    if (dlen < 0 || (size_t)dlen >= sizeof(dest_path))
        dest_path[sizeof(dest_path) - 1] = '\0';

    /* 简单文件复制（生产环境可能是 tar.gz 解压或直接二进制替换） */
    FILE *src = fopen(src_path, "rb");
    if (!src) {
        LOG_ERROR("ota: fopen src %s: %s", src_path, strerror(errno));
        return E_IO;
    }

    FILE *dst = fopen(dest_path, "wb");
    if (!dst) {
        LOG_ERROR("ota: fopen dst %s: %s", dest_path, strerror(errno));
        fclose(src);
        return E_IO;
    }

    char buf[OTA_HTTP_BUF_SIZE];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }

    fclose(src);
    fclose(dst);

    /* 设置可执行权限 */
    chmod(dest_path, 0755);

    LOG_INFO("ota: firmware installed to %s", dest_path);
    return E_OK;
}

/* ─── 槽位切换 ─────────────────────────────────────────── */

static int ota_switch_slot(int slot)
{
    const char *slot_str = (slot == 0) ? "A" : "B";

    if (ota_write_slot_file(OTA_CURRENT_SLOT_FILE, slot_str) != E_OK) {
        return E_IO;
    }

    g_active_slot = slot;
    LOG_INFO("ota: switched to slot %s", slot_str);
    return E_OK;
}

/* ─── OTA 指令 JSON 解析 ────────────────────────────────── */

/*
 * 极简 JSON 解析：提取 upgrade 指令的关键字段
 * 格式: {"cmd":"upgrade","version":"2.0.1","url":"http://...","checksum":"sha256:abc...","force":false}
 */
static int ota_parse_upgrade_cmd(const char *json, int len)
{
    (void)len;

    /* 查找 cmd 字段 */
    const char *p = strstr(json, "\"cmd\"");
    if (!p || !strstr(json, "upgrade")) {
        LOG_ERROR("ota: not an upgrade command");
        return E_INVAL;
    }

    /* 清空目标字段 */
    memset(g_target_version, 0, sizeof(g_target_version));
    memset(g_download_url, 0, sizeof(g_download_url));
    memset(g_expected_checksum, 0, sizeof(g_expected_checksum));

    /* 提取 version */
    p = strstr(json, "\"version\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++; /* skip : */
            while (*p == ' ' || *p == '"') p++;
            int i = 0;
            while (*p && *p != '"' && *p != ',' && i < OTA_VERSION_MAX - 1) {
                g_target_version[i++] = *p++;
            }
            g_target_version[i] = '\0';
        }
    }

    /* 提取 url */
    p = strstr(json, "\"url\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            int i = 0;
            while (*p && *p != '"' && i < OTA_URL_MAX - 1) {
                g_download_url[i++] = *p++;
            }
            g_download_url[i] = '\0';
        }
    }

    /* 提取 checksum */
    p = strstr(json, "\"checksum\"");
    if (p) {
        p = strchr(p, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            int i = 0;
            while (*p && *p != '"' && i < OTA_CHECKSUM_MAX - 1) {
                g_expected_checksum[i++] = *p++;
            }
            g_expected_checksum[i] = '\0';
        }
    }

    /* 校验必要字段 */
    if (g_target_version[0] == '\0' || g_download_url[0] == '\0') {
        LOG_ERROR("ota: upgrade cmd missing version or url");
        return E_INVAL;
    }

    LOG_INFO("ota parsed: version=%s url=%s checksum=%s",
             g_target_version, g_download_url,
             g_expected_checksum[0] ? g_expected_checksum : "(none)");

    return E_OK;
}
