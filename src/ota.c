/*
 * ota.c
 * A/B 分区 OTA 远程升级实现（阶段四）
 *
 * ── 升级流程 ──
 * 1. MQTT 收到 upgrade 指令（JSON）
 * 2. HTTP GET 下载固件到 download/ 目录
 * 3. SHA256 校验固件完整性
 * 4. 解压/复制到备用槽位
 * 5. 更新 current_slot 文件，切换到新槽位（写入 boot_count = 1，启用试用计数）
 * 6. 退出程序（exit(42)），由 systemd 通过 RestartForceExitStatus=42 重启
 * 7. 启动后 post_boot_check（不依赖网络）：
 *    试用计数达 max → 回滚；稳定运行 boot_confirm_sec → confirm 清零
 *
 * ── 安全 ──
 * - SHA256 固件完整性校验（需 libcrypto）
 * - 启动失败自动回滚（boot_attempt ≥ max_attempts 切回旧槽，max 默认 3）
 * - 健康确认机制（稳定运行 boot_confirm_sec 后由主循环周期驱动清零试用计数）
 * - exit(42) → systemd 检测 → 自动重启加载目标槽位
 * - 槽位文件原子写（tmp + fsync + rename + 目录 fsync，掉电不丢切换）
 * - A/B 双槽隔离，升级失败不影响当前运行版本
 *
 * v1.2.7 fail-safe 修复：健康指示器（boot_count / current_slot）的
 * 持久化"写不成宁可不推进状态机"——所有推进动作的前置条件都是
 * ota_write_slot_file / ota_switch_slot 持久化成功；写失败按各调用点
 * 的 fail-safe 策略中止/重试/告警（详见各处 "v1.2.7 fail-safe" 注释）。
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
static time_t         g_boot_time     = 0;          /* ota_init 时刻；用于健康确认计时 */
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

    /* 记录 ota_init 时刻，作为本次启动健康确认的起点 */
    g_boot_time = time(NULL);

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

        /* 新版本进入试用，从 1 开始计数（首次 post_boot_check 读到 1，
         * 0 < 1 < max → 自增到 2；稳定运行 boot_confirm_sec 后 confirm 清零）
         *
         * v1.2.7 fail-safe（缺陷 ①）：boot_count 是回滚判定的命根子。
         * 此刻尚未切槽、尚未 exit(42)；若写 "1" 失败（磁盘满/权限错误）
         * 却继续推进状态机，切槽后新固件会读到旧值（通常是 0=已确认）
         * → 直接报 confirmed → 回滚保护被静默绕过。因此写失败必须
         * 中止安装：上报 error、state=FAILED，留在当前好固件上，
         * 用户/运维可稍后重试升级（该路径由 tests/test_ota.c 用例 C 覆盖）。 */
        if (ota_write_slot_file(OTA_BOOT_COUNT_FILE, "1") != E_OK) {
            LOG_ERROR("ota: failed to persist boot_count=1 "
                      "(disk full or permission error?), aborting install");
            ota_report_status("error",
                              "\"Boot count persist failed, install aborted\"");
            g_state = OTA_STATE_FAILED;
            break;
        }

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

        /* 优雅退出 → 由 systemd RestartForceExitStatus=42 自动重启 */
        LOG_INFO("ota: exiting for reboot (expect systemd to restart)");
        exit(42);
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

/*
 * 启动后健康检查（健康指示器模式）。
 * 流程：
 *   1. 读 boot_count → g_boot_attempt
 *   2. count == 0：已确认健康（或从未升级），上报 running 后返回
 *   3. count >= max：试用期内反复失败，切回旧槽、清零计数，exit(42)
 *   4. 0 < count < max：递增计数后继续尝试（稳定运行后由 ota_confirm_boot 清零）
 * 不依赖 MQTT/网络；调用方负责保证先调 ota_init。
 */
void ota_post_boot_check(void)
{
    if (!g_cfg.enabled)
        return;

    int max_attempts = g_cfg.boot_attempt_max;
    if (max_attempts <= 0)
        max_attempts = OTA_BOOT_ATTEMPT_MAX;

    /* 读取当前计数（ota_init 已读过，这里以文件为准再读一次，外部可能改写）*/
    char count_buf[16] = {0};
    if (ota_read_slot_file(OTA_BOOT_COUNT_FILE, count_buf, sizeof(count_buf)) > 0) {
        g_boot_attempt = atoi(count_buf);
    } else {
        g_boot_attempt = 0;
    }

    /* count == 0：已确认健康（或从未升级），正常启动 */
    if (g_boot_attempt == 0) {
        LOG_INFO("ota: confirmed boot on slot %c",
                 g_active_slot ? 'B' : 'A');
        ota_report_status("running", "\"slot\":\"%c\"",
                          g_active_slot ? 'B' : 'A');
        return;
    }

    /* count >= max：试用期内反复失败 → 回滚 */
    if (g_boot_attempt >= max_attempts) {
        LOG_ERROR("ota: max boot attempts (%d) reached, rolling back!",
                  g_boot_attempt);
        ota_report_status("rollback",
                          "\"reason\":\"boot_failed_%d_times\"",
                          g_boot_attempt);

        int fallback_slot = (g_active_slot == 0) ? 1 : 0;

        /* v1.2.7 fail-safe（缺陷 ②）：切槽失败时绝不能清零计数——
         * 否则下轮重启读到 count 已被清 0，坏固件会被永久"确认"。
         * 保留 count ≥ max，下轮重启 post_boot_check 有机会重试回滚，
         * 反复重启最终由 systemd StartLimitBurst 兜底停止。
         * 为什么仍然 exit(42)（与用户对齐的简单实现）：让 launcher
         * 下一轮重新读 current_slot——即便仍是坏槽，状态机也有机会
         * 重试切槽；与其留在已知会崩的固件里继续跑，不如把控制权
         * 交还给 systemd 重启策略（该路径由 tests/test_ota.c 用例 B 覆盖）。 */
        if (ota_switch_slot(fallback_slot) != E_OK) {
            LOG_ERROR("ota: rollback slot switch to %c failed, "
                      "boot_count kept at %d for retry after reboot",
                      fallback_slot ? 'B' : 'A', g_boot_attempt);
            ota_report_status("error",
                              "\"Rollback slot switch failed\"");
            usleep(500000);
            exit(42);   /* 有意 exit：见上方注释，由 StartLimitBurst 兜底 */
        }

        /* v1.2.7 fail-safe（缺陷 ③）：走到这里切槽已成功，此处清零
         * 失败只告警、不阻断（仍照常 exit(42) 重启到旧槽）。最坏情况：
         * 下次启动 count 仍 ≥ max 会再次触发回滚（切回刚离开的槽），
         * 形成乒乓——同样由 StartLimitBurst 兜底；这与静默绕过回滚
         * 保护相比是更安全的失败方向（由 tests/test_ota.c 用例 B2 覆盖）。 */
        if (ota_write_slot_file(OTA_BOOT_COUNT_FILE, "0") != E_OK) {
            LOG_ERROR("ota: failed to clear boot_count after rollback "
                      "(count stays %d, may re-trigger rollback)",
                      g_boot_attempt);
        } /* 旧固件免试用 */

        LOG_INFO("ota: rolled back to slot %c, rebooting...",
                 fallback_slot ? 'B' : 'A');
        usleep(500000);
        exit(42);   /* 与 RestartForceExitStatus=42 接线 */
    }

    /* 0 < count < max：试用期内，递增后继续尝试 */
    g_boot_attempt++;
    char new_count[16];
    snprintf(new_count, sizeof(new_count), "%d", g_boot_attempt);

    /* v1.2.7 fail-safe（缺陷 ④）：递增失败只告警。最坏情况 count 卡住
     * 永远到不了 max，坏固件反复崩溃直到 StartLimitBurst 耗尽——此处
     * 正运行在（可能已损坏的）试用固件里，除了告警无法做更多。 */
    if (ota_write_slot_file(OTA_BOOT_COUNT_FILE, new_count) != E_OK) {
        LOG_ERROR("ota: failed to persist boot_count=%d, rollback "
                  "threshold may never be reached "
                  "(StartLimitBurst is the last resort)",
                  g_boot_attempt);
    }

    LOG_WARN("ota: trial boot attempt %d/%d on slot %c (confirm in %ds)",
             g_boot_attempt, max_attempts,
             g_active_slot ? 'B' : 'A',
             g_cfg.boot_confirm_sec > 0 ? g_cfg.boot_confirm_sec
                                        : OTA_BOOT_CONFIRM_SEC_DEFAULT);

    ota_report_status("running",
                      "\"slot\":\"%c\",\"boot_attempt\":%d",
                      g_active_slot ? 'B' : 'A', g_boot_attempt);
}

/*
 * 启动健康确认（由主循环每 5s 调一次）：
 *   - 未启用 / 已确认（count == 0）：no-op
 *   - 自上次 ota_init 起的运行时间 ≥ boot_confirm_sec：清零计数、上报 confirmed
 * 稳定运行一段时间后清零，与 ota_post_boot_check 试用递增配合形成
 * "N 次启动失败 → 回滚" 的判定。
 */
void ota_confirm_boot(void)
{
    if (!g_cfg.enabled || g_boot_attempt == 0)
        return;   /* 已确认 / 未启用，no-op */

    int confirm_sec = g_cfg.boot_confirm_sec;
    if (confirm_sec <= 0)
        confirm_sec = OTA_BOOT_CONFIRM_SEC_DEFAULT;

    if (difftime(time(NULL), g_boot_time) < (double)confirm_sec)
        return;   /* 还没稳定运行够久 */

    /* v1.2.7 fail-safe（缺陷 ⑤）：必须先确认文件清零成功，再清内存标志。
     * 旧实现先清 g_boot_attempt：写失败后本函数入口的 count==0 短路
     * 判定会让清零永远不被重试（文件里的 count 清不掉）。保留标志时，
     * 主循环每 5s 调用一次 confirm 会自动重试（由 tests/test_ota.c
     * 用例 A 覆盖）。 */
    if (ota_write_slot_file(OTA_BOOT_COUNT_FILE, "0") != E_OK) {
        LOG_ERROR("ota: failed to clear boot_count, will retry in 5s");
        return;   /* 关键：内存标志保留，5s 后主循环再调 */
    }
    g_boot_attempt = 0;   /* 文件清成功才清内存 */

    LOG_INFO("ota: boot confirmed healthy after %ds, counter cleared",
             confirm_sec);
    ota_report_status("running", "\"slot\":\"%c\",\"confirmed\":true",
                      g_active_slot ? 'B' : 'A');
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
    char path[512], tmp[512];
    snprintf(path, sizeof(path), "%s/%s", g_slot_dir, filename);
    snprintf(tmp,  sizeof(tmp),  "%s/.%s.tmp", g_slot_dir, filename);

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        LOG_ERROR("ota: write %s failed: %s", path, strerror(errno));
        return E_IO;
    }
    fprintf(fp, "%s\n", content);
    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        /* v1.2.7：此前静默返回 E_IO——上层忽略返回值时磁盘故障完全
         * 不可见，这是健康指示器失效无告警的根源。补 LOG_ERROR。
         * 注：fflush/fsync 失败在普通 tmpfs/测试环境难以稳定注入，
         * 该分支未做单测覆盖，由日志告警兜底（见 tests/test_ota.c
         * 用例 D 说明）。 */
        LOG_ERROR("ota: flush/fsync %s failed: %s", tmp, strerror(errno));
        fclose(fp);
        unlink(tmp);
        return E_IO;
    }
    fclose(fp);
    if (rename(tmp, path) != 0) {
        /* v1.2.7：rename 失败（如目标被目录占用 EISDIR、跨设备 EXDEV）
         * 同样补告警（由 tests/test_ota.c 用例 D 覆盖）。 */
        LOG_ERROR("ota: rename %s -> %s failed: %s",
                  tmp, path, strerror(errno));
        unlink(tmp);
        return E_IO;
    }

    /* 目录 fsync：确保 rename 本身落盘（掉电不丢这次切换） */
    int dfd = open(g_slot_dir, O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
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
