/*
 * ota.c
 * A/B 分区 OTA 远程升级实现（阶段四）
 *
 * ── 升级流程 ──
 * 1. MQTT 收到 upgrade 指令（JSON）
 * 2. HTTP(S) GET 下载固件到 download/ 目录，同时下载 <固件URL>.sig 签名文件
 * 3. SHA256 校验固件完整性（快速失败）→ 数字签名验签（硬性关卡）
 * 4. 解压/复制到备用槽位
 * 5. 更新 current_slot 文件，切换到新槽位（写入 boot_count = 1，启用试用计数）
 * 6. 退出程序（exit(42)），由 systemd 通过 RestartForceExitStatus=42 重启
 * 7. 启动后 post_boot_check（不依赖网络）：
 *    试用计数达 max → 回滚；稳定运行 boot_confirm_sec → confirm 清零
 *
 * ── 安全（v1.2.9 威胁模型）──
 * - 固件签名验签（fail-closed，防伪造的根）：
 *   OTA 指令经 MQTT 下发，而 MQTT 通道本身可能是明文的——仅靠指令里带的
 *   SHA256 checksum 防不住"同时伪造指令+固件"的中间人（校验和随指令一起
 *   被替换）。因此完整性之外还需要来自离线私钥的数字签名：
 *     · 公钥路径未配置（空串）→ OTA 升级指令直接拒绝，绝不降级回
 *       checksum-only（用户已确认的 fail-closed 语义）
 *     · 公钥已配置 → 验签是硬性关卡：.sig 下载失败 / 签名不匹配 /
 *       公钥文件读不出 → state=FAILED + 上报 error，绝不切槽
 *   校验顺序：SHA256 checksum（快速失败）→ 数字签名（强校验）
 * - HTTPS 下载（http:// 仍支持，用于局域网自建源场景）：https:// 走
 *   SSL_VERIFY_PEER + CA 锚点（ota_ca_file/ota_ca_path，缺省尝试系统 CA
 *   常见位置，找不到即拒绝）+ SNI + 证书主机名匹配（域名 X509_check_host /
 *   IP 字面量 X509_check_ip_asc）。签名关卡是防伪造的根，传输加密是防
 *   窃听/防固件+签名被整体换包的辅助手段，二者互补不互替。
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
 *
 * v1.2.9（P0-5 + P1-14）：
 * - 固件签名验签（ota_verify_signature，独立可测）+ .sig 下载
 * - HTTPS 下载支持（OpenSSL，-lssl）
 * - P1-14：响应头解析改为累积缓冲 + NUL 终止后查找（修复跨 TCP 段/TLS
 *   记录的头部切分错误、读未初始化栈内存的 UB、body 字节丢失）
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
#include <pthread.h>
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>

/* ─── 内部常量 ─────────────────────────────────────────── */

#define OTA_STATUS_TOPIC_FMT  "embmqttnode/%s/ota/status"
#define OTA_CMD_TOPIC_FMT     "embmqttnode/%s/ota/cmd"
#define OTA_DOWNLOAD_SUBDIR   "download"
#define OTA_FIRMWARE_FILE     "firmware.bin"
#define OTA_SIG_SUFFIX        ".sig"
#define OTA_CURRENT_SLOT_FILE "current_slot"
#define OTA_BOOT_COUNT_FILE   "boot_count"
#define OTA_HTTP_BUF_SIZE     4096
#define OTA_HTTP_HEADER_MAX   8192    /* 响应头累积缓冲上限（防恶意超长头） */
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

/*
 * P1-3: OTA 状态机跨线程同步。
 * 修复前 g_state / g_target_* 等只在网络线程（handle_message）与
 * 上报线程（check_and_handle）之间靠"运气"传递——无任何同步原语，
 * 属数据竞争 UB（另一处是 mqtt_client 的 g_connected）。
 * 修复方案：pthread 互斥锁保护全部状态机字段。
 *   · 锁内只做内存读写（快照/提交），绝不做网络/文件 I/O——
 *     下载一卡几十秒不能拖死 ota_state_string 等查询方；
 *   · 每步流程 = 锁内快照输入 → 解锁干活 → 回锁提交 → 解锁后上报
 *     （ota_report_status 自身会加锁，调用方不得持锁调用，防死锁）。
 */
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;

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

/* P1-3: 状态机各步独立函数（快照-干活-提交模式，锁内无 I/O） */
static void ota_step_downloading(void);
static void ota_step_verifying(void);
static void ota_step_installing(void);
static void ota_step_rebooting(void);
static void ota_step_failed(void);

/* P1-3: 由已快照的状态枚举取名（供已持锁快照的调用方使用，
 * 避免在持锁路径调用会加锁的 ota_state_string 造成死锁） */
static const char *ota_state_string_from(enum ota_state s);

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

    /*
     * P1-3: 状态机字段全部由 g_state_lock 保护。
     * 解析（写 g_target_*）与推进 DOWNLOADING 都在锁内完成；
     * ota_report_status / ota_state_string 自身会加锁，
     * 因此所有上报/查询都放在解锁之后（防死锁）。
     */
    pthread_mutex_lock(&g_state_lock);
    if (g_state != OTA_STATE_IDLE) {
        enum ota_state snap = g_state;
        pthread_mutex_unlock(&g_state_lock);
        LOG_WARN("ota: busy (state=%s), ignoring command",
                 ota_state_string_from(snap));
        return;
    }

    if (ota_parse_upgrade_cmd(payload, payload_len) != E_OK) {
        pthread_mutex_unlock(&g_state_lock);
        ota_report_status("error", "\"Invalid OTA command JSON\"");
        return;
    }

    /*
     * v1.2.9 fail-closed（用户已确认的语义，勿改为降级）：
     * 签名公钥未配置 → 直接拒绝升级指令。绝不降级回 checksum-only——
     * MQTT 通道可能是明文的，checksum 随指令可被中间人一并伪造，没有
     * 离线签名把关的 OTA 等于向全网开放刷机口。
     */
    if (g_cfg.public_key[0] == '\0') {
        pthread_mutex_unlock(&g_state_lock);
        LOG_ERROR("ota: upgrade rejected: ota_public_key not configured "
                  "(fail-closed: unsigned firmware install is not allowed)");
        ota_report_status("error",
                          "\"Firmware public key not configured, "
                          "upgrade rejected (fail-closed)\"");
        return;
    }

    char received_ver[OTA_VERSION_MAX];
    snprintf(received_ver, sizeof(received_ver), "%s", g_target_version);
    g_state = OTA_STATE_DOWNLOADING;
    pthread_mutex_unlock(&g_state_lock);

    LOG_INFO("ota: upgrade cmd received version=%s", received_ver);
}

/*
 * P1-3/P1-10: 状态机推进（由独立 OTA worker 线程周期调用）。
 * 只做分发：锁内快照当前状态 → 解锁 → 派发到对应 step 函数。
 * 每个 step 遵循"锁内快照输入 → 解锁干活（网络/文件 I/O）→
 * 回锁提交状态 → 解锁后上报"——锁内绝不出现 I/O。
 */
int ota_check_and_handle(void)
{
    if (!g_cfg.enabled)
        return 0;

    pthread_mutex_lock(&g_state_lock);
    enum ota_state snap = g_state;
    pthread_mutex_unlock(&g_state_lock);

    if (snap == OTA_STATE_IDLE)
        return 0;

    switch (snap) {
    case OTA_STATE_DOWNLOADING: ota_step_downloading(); break;
    case OTA_STATE_VERIFYING:   ota_step_verifying();   break;
    case OTA_STATE_INSTALLING:  ota_step_installing();  break;
    case OTA_STATE_REBOOTING:   ota_step_rebooting();   break;
    case OTA_STATE_FAILED:      ota_step_failed();      break;
    default: break;
    }

    pthread_mutex_lock(&g_state_lock);
    int busy = (g_state != OTA_STATE_IDLE);
    pthread_mutex_unlock(&g_state_lock);
    return busy;
}

/* ── DOWNLOADING：下载固件 + .sig（网络 I/O，全程不持锁）── */

static void ota_step_downloading(void)
{
    char url[OTA_URL_MAX];
    char dest_path[512];

    pthread_mutex_lock(&g_state_lock);
    if (g_state != OTA_STATE_DOWNLOADING) {
        /* 状态已被其他路径改写（理论上不会发生），保守放弃本步 */
        pthread_mutex_unlock(&g_state_lock);
        return;
    }
    snprintf(url, sizeof(url), "%s", g_download_url);
    pthread_mutex_unlock(&g_state_lock);

    snprintf(dest_path, sizeof(dest_path), "%s/%s/%s",
             g_slot_dir, OTA_DOWNLOAD_SUBDIR, OTA_FIRMWARE_FILE);

    ota_report_status("downloading", NULL);
    LOG_INFO("ota: downloading %s → %s", url, dest_path);

    if (ota_http_download(url, dest_path) != E_OK) {
        ota_report_status("error", "\"HTTP download failed\"");
        pthread_mutex_lock(&g_state_lock);
        g_state = OTA_STATE_FAILED;
        pthread_mutex_unlock(&g_state_lock);
        return;
    }

    /*
     * v1.2.9 fail-closed：同时下载签名文件 <固件URL>.sig。
     * 公钥已配置（ota_handle_message 已把关），.sig 拿不到就是
     * 异常事件（中间人剥离签名 / 源站部署不全），必须拒绝安装。
     */
    char sig_url[OTA_URL_MAX + 4];
    int surl = snprintf(sig_url, sizeof(sig_url),
                        "%s%s", url, OTA_SIG_SUFFIX);
    if (surl < 0 || (size_t)surl >= sizeof(sig_url)) {
        LOG_ERROR("ota: signature URL too long");
        ota_report_status("error", "\"Signature URL too long\"");
        pthread_mutex_lock(&g_state_lock);
        g_state = OTA_STATE_FAILED;
        pthread_mutex_unlock(&g_state_lock);
        return;
    }
    char sig_path[sizeof(dest_path) + 4];   /* dest_path + ".sig" */
    snprintf(sig_path, sizeof(sig_path), "%s%s", dest_path,
             OTA_SIG_SUFFIX);

    LOG_INFO("ota: downloading signature %s → %s", sig_url, sig_path);
    if (ota_http_download(sig_url, sig_path) != E_OK) {
        LOG_ERROR("ota: signature file download failed, refusing install");
        ota_report_status("error", "\"Signature file download failed\"");
        pthread_mutex_lock(&g_state_lock);
        g_state = OTA_STATE_FAILED;
        pthread_mutex_unlock(&g_state_lock);
        return;
    }

    pthread_mutex_lock(&g_state_lock);
    g_download_pct = 100;
    g_state = OTA_STATE_VERIFYING;
    pthread_mutex_unlock(&g_state_lock);
}

/* ── VERIFYING：SHA256 + 验签（文件 I/O，全程不持锁）──── */

static void ota_step_verifying(void)
{
    char expected_checksum[OTA_CHECKSUM_MAX];
    char public_key[512];

    pthread_mutex_lock(&g_state_lock);
    if (g_state != OTA_STATE_VERIFYING) {
        pthread_mutex_unlock(&g_state_lock);
        return;
    }
    snprintf(expected_checksum, sizeof(expected_checksum), "%s",
             g_expected_checksum);
    snprintf(public_key, sizeof(public_key), "%s", g_cfg.public_key);
    pthread_mutex_unlock(&g_state_lock);

    char hash_str[128] = {0};
    char src_path[512];
    char sig_path[sizeof(src_path) + 4];    /* src_path + ".sig" */
    snprintf(src_path, sizeof(src_path), "%s/%s/%s",
             g_slot_dir, OTA_DOWNLOAD_SUBDIR, OTA_FIRMWARE_FILE);
    snprintf(sig_path, sizeof(sig_path), "%s%s", src_path,
             OTA_SIG_SUFFIX);

    ota_report_status("verifying", NULL);

    /* ── 第一道关卡：SHA256 checksum（快速失败） ── */
    if (ota_sha256_file(src_path, hash_str, sizeof(hash_str)) != E_OK) {
        ota_report_status("error", "\"SHA256 computation failed\"");
        pthread_mutex_lock(&g_state_lock);
        g_state = OTA_STATE_FAILED;
        pthread_mutex_unlock(&g_state_lock);
        return;
    }

    LOG_INFO("ota: sha256 computed = %s", hash_str);
    LOG_INFO("ota: sha256 expected = %s", expected_checksum);

    /* 比较校验和（兼容带 "sha256:" 前缀的格式） */
    const char *exp = expected_checksum;
    if (strncmp(exp, "sha256:", 7) == 0)
        exp += 7;

    if (strcasecmp(hash_str, exp) != 0) {
        LOG_ERROR("ota: checksum mismatch!");
        ota_report_status("error", "\"Checksum mismatch\"");
        pthread_mutex_lock(&g_state_lock);
        g_state = OTA_STATE_FAILED;
        pthread_mutex_unlock(&g_state_lock);
        return;
    }

    ota_report_status("verifying", "\"checksum_ok\":true");

    /*
     * ── 第二道关卡：数字签名验签（v1.2.9，硬性 fail-closed） ──
     * checksum 只证明"固件与指令一致"，防不住指令+固件被中间人
     * 一并伪造；签名来自离线私钥，才是拒绝伪造固件的根。
     * 验签失败 / .sig 缺失 / 公钥读不出 → 一律 FAILED，绝不切槽。
     */
    if (ota_verify_signature(src_path, sig_path, public_key) != E_OK) {
        LOG_ERROR("ota: signature verification failed, refusing install");
        ota_report_status("error", "\"Signature verification failed\"");
        pthread_mutex_lock(&g_state_lock);
        g_state = OTA_STATE_FAILED;
        pthread_mutex_unlock(&g_state_lock);
        return;
    }

    ota_report_status("verifying", "\"signature_ok\":true");
    pthread_mutex_lock(&g_state_lock);
    g_state = OTA_STATE_INSTALLING;
    pthread_mutex_unlock(&g_state_lock);
}

/* ── INSTALLING：写备用槽 + 切槽（文件 I/O，全程不持锁）─ */

static void ota_step_installing(void)
{
    int target_slot;

    pthread_mutex_lock(&g_state_lock);
    if (g_state != OTA_STATE_INSTALLING) {
        pthread_mutex_unlock(&g_state_lock);
        return;
    }
    /* 目标槽位 = 非当前槽 */
    target_slot = (g_active_slot == 0) ? 1 : 0;
    g_target_slot = target_slot;   /* 提前登记，REBOOTING 日志要用 */
    pthread_mutex_unlock(&g_state_lock);

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/%s/%s",
             g_slot_dir, OTA_DOWNLOAD_SUBDIR, OTA_FIRMWARE_FILE);

    ota_report_status("installing",
                      "\"target_slot\":\"%c\"",
                      target_slot ? 'B' : 'A');

    if (ota_install_firmware(src_path, target_slot) != E_OK) {
        ota_report_status("error", "\"Installation failed\"");
        pthread_mutex_lock(&g_state_lock);
        g_state = OTA_STATE_FAILED;
        pthread_mutex_unlock(&g_state_lock);
        return;
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
        pthread_mutex_lock(&g_state_lock);
        g_state = OTA_STATE_FAILED;
        pthread_mutex_unlock(&g_state_lock);
        return;
    }

    /* 切换活动槽位（内部文件写在锁外，g_active_slot 更新在锁内） */
    if (ota_switch_slot(target_slot) != E_OK) {
        ota_report_status("error", "\"Slot switch failed\"");
        pthread_mutex_lock(&g_state_lock);
        g_state = OTA_STATE_FAILED;
        pthread_mutex_unlock(&g_state_lock);
        return;
    }

    pthread_mutex_lock(&g_state_lock);
    g_state = OTA_STATE_REBOOTING;
    pthread_mutex_unlock(&g_state_lock);
}

/* ── REBOOTING：上报后 exit(42) 交由 systemd 重启 ───────── */

static void ota_step_rebooting(void)
{
    pthread_mutex_lock(&g_state_lock);
    int target_slot = g_target_slot;
    pthread_mutex_unlock(&g_state_lock);

    ota_report_status("rebooting", NULL);
    LOG_INFO("ota: rebooting to slot %c...",
             target_slot ? 'B' : 'A');

    /* 给 MQTT 一点时间发送状态消息 */
    usleep(500000);

    /* 优雅退出 → 由 systemd RestartForceExitStatus=42 自动重启 */
    LOG_INFO("ota: exiting for reboot (expect systemd to restart)");
    exit(42);
}

/* ── FAILED：复位回 IDLE，等待下一条指令 ────────────────── */

static void ota_step_failed(void)
{
    pthread_mutex_lock(&g_state_lock);
    g_state = OTA_STATE_IDLE;
    pthread_mutex_unlock(&g_state_lock);
    LOG_WARN("ota: in failed state, resetting to IDLE");
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
    int file_attempt = 0;
    if (ota_read_slot_file(OTA_BOOT_COUNT_FILE, count_buf, sizeof(count_buf)) > 0) {
        file_attempt = atoi(count_buf);
    }
    pthread_mutex_lock(&g_state_lock);
    g_boot_attempt = file_attempt;
    int attempt = g_boot_attempt;
    int active_slot = g_active_slot;
    pthread_mutex_unlock(&g_state_lock);

    /* count == 0：已确认健康（或从未升级），正常启动 */
    if (attempt == 0) {
        LOG_INFO("ota: confirmed boot on slot %c",
                 active_slot ? 'B' : 'A');
        ota_report_status("running", "\"slot\":\"%c\"",
                          active_slot ? 'B' : 'A');
        return;
    }

    /* count >= max：试用期内反复失败 → 回滚 */
    if (attempt >= max_attempts) {
        LOG_ERROR("ota: max boot attempts (%d) reached, rolling back!",
                  attempt);
        ota_report_status("rollback",
                          "\"reason\":\"boot_failed_%d_times\"",
                          attempt);

        int fallback_slot = (active_slot == 0) ? 1 : 0;

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
                      attempt);
        } /* 旧固件免试用 */

        LOG_INFO("ota: rolled back to slot %c, rebooting...",
                 fallback_slot ? 'B' : 'A');
        usleep(500000);
        exit(42);   /* 与 RestartForceExitStatus=42 接线 */
    }

    /* 0 < count < max：试用期内，递增后继续尝试 */
    pthread_mutex_lock(&g_state_lock);
    g_boot_attempt++;
    attempt = g_boot_attempt;
    active_slot = g_active_slot;
    pthread_mutex_unlock(&g_state_lock);
    char new_count[16];
    snprintf(new_count, sizeof(new_count), "%d", attempt);

    /* v1.2.7 fail-safe（缺陷 ④）：递增失败只告警。最坏情况 count 卡住
     * 永远到不了 max，坏固件反复崩溃直到 StartLimitBurst 耗尽——此处
     * 正运行在（可能已损坏的）试用固件里，除了告警无法做更多。 */
    if (ota_write_slot_file(OTA_BOOT_COUNT_FILE, new_count) != E_OK) {
        LOG_ERROR("ota: failed to persist boot_count=%d, rollback "
                  "threshold may never be reached "
                  "(StartLimitBurst is the last resort)",
                  attempt);
    }

    LOG_WARN("ota: trial boot attempt %d/%d on slot %c (confirm in %ds)",
             attempt, max_attempts,
             active_slot ? 'B' : 'A',
             g_cfg.boot_confirm_sec > 0 ? g_cfg.boot_confirm_sec
                                        : OTA_BOOT_CONFIRM_SEC_DEFAULT);

    ota_report_status("running",
                      "\"slot\":\"%c\",\"boot_attempt\":%d",
                      active_slot ? 'B' : 'A', attempt);
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
    /* P1-3: 锁内快照判定依据（g_boot_attempt / g_boot_time 由锁保护） */
    pthread_mutex_lock(&g_state_lock);
    int attempt = g_boot_attempt;
    time_t boot_time = g_boot_time;
    pthread_mutex_unlock(&g_state_lock);

    if (!g_cfg.enabled || attempt == 0)
        return;   /* 已确认 / 未启用，no-op */

    int confirm_sec = g_cfg.boot_confirm_sec;
    if (confirm_sec <= 0)
        confirm_sec = OTA_BOOT_CONFIRM_SEC_DEFAULT;

    if (difftime(time(NULL), boot_time) < (double)confirm_sec)
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

    pthread_mutex_lock(&g_state_lock);
    /* 提交前复核：文件清零成功，但内存计数若已被其他线程改写
     * （理论上不会），保留新值更安全 */
    if (g_boot_attempt == attempt)
        g_boot_attempt = 0;   /* 文件清成功才清内存 */
    pthread_mutex_unlock(&g_state_lock);

    LOG_INFO("ota: boot confirmed healthy after %ds, counter cleared",
             confirm_sec);
    pthread_mutex_lock(&g_state_lock);
    int active_slot = g_active_slot;
    pthread_mutex_unlock(&g_state_lock);
    ota_report_status("running", "\"slot\":\"%c\",\"confirmed\":true",
                      active_slot ? 'B' : 'A');
}

/* P1-3: 由已快照的状态枚举取名（无锁，配合调用方快照使用） */
static const char *ota_state_string_from(enum ota_state s)
{
    switch (s) {
    case OTA_STATE_IDLE:        return "idle";
    case OTA_STATE_DOWNLOADING: return "downloading";
    case OTA_STATE_VERIFYING:   return "verifying";
    case OTA_STATE_INSTALLING:  return "installing";
    case OTA_STATE_REBOOTING:   return "rebooting";
    case OTA_STATE_FAILED:      return "failed";
    default:                    return "unknown";
    }
}

const char *ota_state_string(void)
{
    /* P1-3: 锁内快照后再取名，g_state 的读取全部经锁 */
    pthread_mutex_lock(&g_state_lock);
    enum ota_state snap = g_state;
    pthread_mutex_unlock(&g_state_lock);
    return ota_state_string_from(snap);
}

void ota_close(void)
{
    pthread_mutex_lock(&g_state_lock);
    g_state = OTA_STATE_IDLE;
    pthread_mutex_unlock(&g_state_lock);
    g_mqtt_publish = NULL;
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

    /*
     * P1-3: 锁内快照要上报的状态字段（g_target_version /
     * g_current_version / g_download_pct 都由锁保护），随后解锁
     * 再拼 payload 与发布——上层（step 函数）保证不持锁调用本函数，
     * 此处加锁不会与调用方形成死锁。
     */
    char version[OTA_VERSION_MAX];
    int pct;
    pthread_mutex_lock(&g_state_lock);
    if (g_target_version[0] != '\0')
        snprintf(version, sizeof(version), "%s", g_target_version);
    else
        snprintf(version, sizeof(version), "%s", g_current_version);
    pct = g_download_pct;
    pthread_mutex_unlock(&g_state_lock);

    char payload[512];
    int off = snprintf(payload, sizeof(payload),
                       "{\"state\":\"%s\",\"version\":\"%s\"",
                       state, version);

    if (pct > 0 && pct < 100) {
        off += snprintf(payload + off, sizeof(payload) - off,
                        ",\"progress\":%d", pct);
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

/* ─── HTTP(S) GET 下载（v1.2.9 重写）────────────────────── */

/*
 * URL 解析结果
 */
struct ota_url_parts {
    int  use_tls;               /* 0=http 1=https */
    char host[256];             /* 主机名或 IP 字面量（不含端口） */
    int  port;                  /* 端口（http 默认 80 / https 默认 443） */
    char path[512];             /* 请求路径（以 / 开头） */
};

/*
 * 解析 http(s)://host[:port]/path。
 * 返回 E_OK 成功；E_INVAL 协议不支持或格式非法。
 */
static int ota_parse_url(const char *url, struct ota_url_parts *out)
{
    if (!url || !out)
        return E_INVAL;

    /* v1.2.9：https 支持加入；http 保留（局域网自建源场景），
     * 防伪造由固件签名关卡兜底，见文件头威胁模型注释。 */
    if (strncmp(url, "http://", 7) == 0) {
        out->use_tls = 0;
        out->port    = 80;
        url += 7;
    } else if (strncmp(url, "https://", 8) == 0) {
        out->use_tls = 1;
        out->port    = 443;
        url += 8;
    } else {
        LOG_ERROR("ota: unsupported URL scheme (need http:// or https://)");
        return E_INVAL;
    }

    const char *slash    = strchr(url, '/');
    size_t      host_len = slash ? (size_t)(slash - url) : strlen(url);
    if (host_len == 0 || host_len >= sizeof(out->host))
        return E_INVAL;
    memcpy(out->host, url, host_len);
    out->host[host_len] = '\0';

    /* host 里带端口则拆出 */
    char *colon = strchr(out->host, ':');
    if (colon) {
        *colon      = '\0';
        out->port   = atoi(colon + 1);
        if (out->port <= 0 || out->port > 65535)
            return E_INVAL;
    }
    if (out->host[0] == '\0')
        return E_INVAL;

    snprintf(out->path, sizeof(out->path), "%s", slash ? slash : "/");
    return E_OK;
}

/*
 * 传输抽象：send/recv 收敛到 conn_* 包装层，http（裸 socket）与
 * https（SSL）共用同一套请求构造 / 响应解析逻辑。
 */
struct ota_conn {
    int      sock;
    SSL     *ssl;                   /* https 时非 NULL */
    SSL_CTX *ctx;                   /* 持有 SSL_CTX，conn_close 时释放 */
};

static int conn_send_all(struct ota_conn *c, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        if (c->ssl) {
            int n = SSL_write(c->ssl, buf + off, (int)(len - off));
            if (n <= 0)
                return E_NET;
            off += (size_t)n;
        } else {
            ssize_t n = send(c->sock, buf + off, len - off, 0);
            if (n <= 0)
                return E_NET;
            off += (size_t)n;
        }
    }
    return E_OK;
}

/* 返回 >0 收到的字节数，0 = 对端正常关闭，<0 = 错误 */
static int conn_recv(struct ota_conn *c, char *buf, size_t size)
{
    if (c->ssl) {
        int n = SSL_read(c->ssl, buf, (int)size);
        if (n > 0)
            return n;
        int err = SSL_get_error(c->ssl, n);
        return (err == SSL_ERROR_ZERO_RETURN) ? 0 : -1;
    }
    for (;;) {
        ssize_t n = recv(c->sock, buf, size, 0);
        if (n > 0)
            return (int)n;
        if (n == 0)
            return 0;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static void conn_close(struct ota_conn *c)
{
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        c->ssl = NULL;
    }
    if (c->ctx) {
        SSL_CTX_free(c->ctx);
        c->ctx = NULL;
    }
    if (c->sock >= 0) {
        close(c->sock);
        c->sock = -1;
    }
}

/*
 * 证书主机名匹配：域名走 X509_check_host，IP 字面量走 X509_check_ip_asc
 * （X509_check_host 不校验 IP 形式的 SAN，需分流）。
 */
static int ota_cert_matches_host(X509 *cert, const char *host)
{
    struct in_addr  v4;
    struct in6_addr v6;
    if (inet_pton(AF_INET, host, &v4) == 1 ||
        inet_pton(AF_INET6, host, &v6) == 1)
        return X509_check_ip_asc(cert, host, 0) == 1;
    return X509_check_host(cert, host, strlen(host), 0, NULL) == 1;
}

/*
 * 在已连接的 socket 上完成 TLS 握手 + 证书校验（fail-closed）：
 *   - SSL_VERIFY_PEER：证书链必须能验证到配置的 CA 锚点
 *   - CA 锚点：ota_ca_file / ota_ca_path 优先；均未配置时尝试系统 CA
 *     常见位置；一个都找不到 → 拒绝连接（绝不裸奔）
 *   - SNI：SSL_set_tlsext_host_name（虚拟主机场景）
 *   - 主机名：握手后显式 X509_check_host / X509_check_ip_asc
 * 成功时填充 c->ssl / c->ctx；失败返回 E_NET/E_IO/E_NO_MEM。
 */
static int ota_tls_connect(struct ota_conn *c, const char *host)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        LOG_ERROR("ota: SSL_CTX_new failed");
        return E_NO_MEM;
    }

    /* 硬性证书校验，绝不使用 SSL_VERIFY_NONE */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    const char *ca_file = g_cfg.ca_file[0] ? g_cfg.ca_file : NULL;
    const char *ca_path = g_cfg.ca_path[0] ? g_cfg.ca_path : NULL;

    if (ca_file || ca_path) {
        if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_path) != 1) {
            LOG_ERROR("ota: load verify locations failed (file=%s path=%s)",
                      ca_file ? ca_file : "(none)",
                      ca_path ? ca_path : "(none)");
            ERR_clear_error();
            SSL_CTX_free(ctx);
            return E_IO;
        }
    } else {
        /* 系统默认 CA 常见位置（嵌入式发行版差异大，逐个探测） */
        static const char *const sys_ca[] = {
            "/etc/ssl/certs/ca-certificates.crt",   /* Debian/Ubuntu */
            "/etc/pki/tls/certs/ca-bundle.crt",     /* Fedora/RHEL */
            "/etc/ssl/ca-bundle.pem",               /* OpenSUSE */
            "/etc/ssl/cert.pem",                    /* macOS/BSD/Alpine */
        };
        int loaded = 0;
        for (size_t i = 0; i < sizeof(sys_ca) / sizeof(sys_ca[0]); i++) {
            if (access(sys_ca[i], R_OK) == 0 &&
                SSL_CTX_load_verify_locations(ctx, sys_ca[i], NULL) == 1) {
                LOG_INFO("ota: using system CA bundle %s", sys_ca[i]);
                loaded = 1;
                break;
            }
        }
        if (!loaded) {
            LOG_ERROR("ota: no system CA bundle found, refusing HTTPS "
                      "without certificate verification (fail-closed)");
            ERR_clear_error();
            SSL_CTX_free(ctx);
            return E_IO;
        }
    }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        ERR_clear_error();
        SSL_CTX_free(ctx);
        return E_NO_MEM;
    }

    /* SNI：让服务端按主机名返回正确证书 */
    SSL_set_tlsext_host_name(ssl, host);

    SSL_set_fd(ssl, c->sock);
    if (SSL_connect(ssl) != 1) {
        LOG_ERROR("ota: TLS handshake failed with %s "
                  "(cert chain or protocol problem)", host);
        ERR_clear_error();
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return E_NET;
    }

    /* SSL_VERIFY_PEER 已保证证书链有效；主机名匹配在此显式把关 */
    X509 *cert = SSL_get_peer_certificate(ssl);
    if (!cert) {
        LOG_ERROR("ota: peer presented no certificate");
        ERR_clear_error();
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return E_NET;
    }
    int host_ok = ota_cert_matches_host(cert, host);
    X509_free(cert);
    if (!host_ok) {
        LOG_ERROR("ota: certificate does not match host %s", host);
        ERR_clear_error();
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        return E_NET;
    }

    c->ssl = ssl;
    c->ctx = ctx;
    LOG_INFO("ota: TLS established with %s (peer cert verified)", host);
    return E_OK;
}

/*
 * HTTP(S) GET 下载。
 * v1.2.9（P1-14）响应头解析重写：
 *   - 累积缓冲：头可能拆在多个 TCP 段 / TLS 记录里到达，逐段追加到
 *     hdr 缓冲、NUL 终止后再查找 "\r\n\r\n"（旧实现只在单个 recv 缓冲
 *     内 strstr，跨段头永远切不开，且 recv 后未 NUL 终止就 strchr /
 *     strstr 是读未初始化栈内存的 UB）
 *   - 分隔符之后落在同一缓冲里的 body 字节直接写入文件，不丢字节
 *   - 状态行解析基于完整累积缓冲
 */
static int ota_http_download(const char *url, const char *dest_path)
{
    struct ota_url_parts up;
    if (ota_parse_url(url, &up) != E_OK)
        return E_NET;

    LOG_INFO("ota: %s GET host=%s port=%d path=%s",
             up.use_tls ? "https" : "http", up.host, up.port, up.path);

    /* DNS 解析 */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", up.port);

    if (getaddrinfo(up.host, port_str, &hints, &res) != 0) {
        LOG_ERROR("ota: DNS lookup failed for %s", up.host);
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

    struct ota_conn conn = { .sock = sock, .ssl = NULL, .ctx = NULL };

    if (up.use_tls && ota_tls_connect(&conn, up.host) != E_OK) {
        conn_close(&conn);
        return E_NET;
    }

    /* 发送 HTTP GET 请求（http/https 共用） */
    char request[1024];
    int req_len = snprintf(request, sizeof(request),
                           "GET %s HTTP/1.0\r\n"
                           "Host: %s\r\n"
                           "User-Agent: EmbMQTTNode-OTA/1.1\r\n"
                           "Connection: close\r\n"
                           "\r\n",
                           up.path, up.host);

    if (conn_send_all(&conn, request, (size_t)req_len) != E_OK) {
        LOG_ERROR("ota: send() failed");
        conn_close(&conn);
        return E_NET;
    }

    /* 接收响应并写入文件 */
    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        LOG_ERROR("ota: fopen %s failed: %s", dest_path, strerror(errno));
        conn_close(&conn);
        return E_IO;
    }

    char hdr[OTA_HTTP_HEADER_MAX];  /* 响应头累积缓冲（P1-14） */
    char buf[OTA_HTTP_BUF_SIZE];
    int  hdr_len     = 0;
    int  header_done = 0;
    int  total_bytes = 0;
    int  status_code = 0;
    int  io_failed   = 0;

    while (!header_done) {
        int n = conn_recv(&conn, buf, sizeof(buf));
        if (n < 0) {
            LOG_ERROR("ota: recv header failed");
            io_failed = 1;
            break;
        }
        if (n == 0) {
            LOG_ERROR("ota: connection closed before header complete");
            io_failed = 1;
            break;
        }

        /* 防恶意超长头：缓冲将满仍未见分隔符则放弃 */
        int space = (int)sizeof(hdr) - 1 - hdr_len;
        if (n > space) {
            if (space == 0) {
                LOG_ERROR("ota: HTTP header too large (>%d bytes)",
                          (int)sizeof(hdr) - 1);
                io_failed = 1;
                break;
            }
            n = space;
        }

        memcpy(hdr + hdr_len, buf, (size_t)n);
        hdr_len += n;
        hdr[hdr_len] = '\0';    /* P1-14：先 NUL 终止再查找 */

        char *sep = strstr(hdr, "\r\n\r\n");
        if (sep) {
            header_done = 1;

            /* 状态行解析基于完整累积缓冲 */
            if (strncmp(hdr, "HTTP/", 5) == 0) {
                char *sp = strchr(hdr, ' ');
                if (sp)
                    status_code = atoi(sp + 1);
            }

            int header_total = (int)(sep - hdr) + 4;
            int body_bytes   = hdr_len - header_total;
            if (body_bytes > 0) {
                /* 头尾相连落进缓冲的 body 字节：不丢 */
                fwrite(sep + 4, 1, (size_t)body_bytes, fp);
                total_bytes += body_bytes;
            }
        } else if (hdr_len >= (int)sizeof(hdr) - 1) {
            LOG_ERROR("ota: HTTP header too large (>%d bytes)",
                      (int)sizeof(hdr) - 1);
            io_failed = 1;
            break;
        }
    }

    while (!io_failed) {
        int n = conn_recv(&conn, buf, sizeof(buf));
        if (n < 0) {
            LOG_ERROR("ota: recv body failed");
            io_failed = 1;
            break;
        }
        if (n == 0)
            break;      /* Connection: close → 读到对端关闭即完成 */
        fwrite(buf, 1, (size_t)n, fp);
        total_bytes += n;
    }

    fclose(fp);
    conn_close(&conn);

    if (io_failed) {
        unlink(dest_path);
        return E_NET;
    }

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

/* ─── 固件签名验签（v1.2.9，独立可测）────────────────────── */

/*
 * 用 PEM 公钥验证 .sig 签名文件对固件文件的签名。
 *
 * 算法支持（OpenSSL EVP 通用验签，1.1.x 兼容，不用 3.x 独有 API）：
 *   - RSA / EC 公钥：SHA256 摘要签名，固件流式喂入（不整载内存）
 *   - Ed25519      ：原生签名（无预摘要），固件整载内存一次性验证
 *
 * 返回 E_OK 验签通过；其他值 = 参数错 / 文件缺失不可读 / 公钥格式错 /
 * 签名不匹配。所有失败路径调用方都必须拒绝安装（fail-closed）。
 */
int ota_verify_signature(const char *fw_path,
                         const char *sig_path,
                         const char *pubkey_path)
{
    if (!fw_path || !sig_path || !pubkey_path || pubkey_path[0] == '\0') {
        LOG_ERROR("ota: verify_signature: invalid args");
        return E_INVAL;
    }

    /* 1. 加载公钥（PEM "PUBLIC KEY"，RSA/EC/Ed25519 通用） */
    FILE *kfp = fopen(pubkey_path, "r");
    if (!kfp) {
        LOG_ERROR("ota: open public key %s failed: %s",
                  pubkey_path, strerror(errno));
        return E_IO;
    }
    EVP_PKEY *pkey = PEM_read_PUBKEY(kfp, NULL, NULL, NULL);
    fclose(kfp);
    if (!pkey) {
        LOG_ERROR("ota: parse public key %s failed (not a PEM PUBLIC KEY?)",
                  pubkey_path);
        ERR_clear_error();
        return E_IO;
    }

    /* 2. 读取签名文件（原始签名字节，不做 base64） */
    FILE *sfp = fopen(sig_path, "rb");
    if (!sfp) {
        LOG_ERROR("ota: open signature %s failed: %s",
                  sig_path, strerror(errno));
        EVP_PKEY_free(pkey);
        return E_NOT_FOUND;
    }
    unsigned char sig_buf[4096];
    size_t sig_len = fread(sig_buf, 1, sizeof(sig_buf), sfp);
    int sig_trunc = fgetc(sfp) != EOF;   /* 超过缓冲视为异常 */
    fclose(sfp);
    if (sig_len == 0 || sig_trunc) {
        LOG_ERROR("ota: signature %s empty or too large (%zu bytes)",
                  sig_path, sig_len);
        EVP_PKEY_free(pkey);
        return E_IO;
    }

    int  is_ed25519 = (EVP_PKEY_id(pkey) == EVP_PKEY_ED25519);
    int  ok         = 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return E_NO_MEM;
    }

    if (is_ed25519) {
        /* Ed25519：原生一次性签名，无 EVP_DigestSign/Verify 前缀 */
        FILE *ffp = fopen(fw_path, "rb");
        if (!ffp) {
            LOG_ERROR("ota: open firmware %s failed: %s",
                      fw_path, strerror(errno));
            EVP_MD_CTX_free(ctx);
            EVP_PKEY_free(pkey);
            return E_IO;
        }
        /* 整载固件（网关固件体量 MB 级，可接受） */
        if (fseek(ffp, 0, SEEK_END) == 0) {
            long fsize = ftell(ffp);
            rewind(ffp);
            if (fsize > 0) {
                unsigned char *fw = malloc((size_t)fsize);
                if (fw) {
                    size_t got = fread(fw, 1, (size_t)fsize, ffp);
                    if (got == (size_t)fsize &&
                        EVP_DigestVerifyInit(ctx, NULL, NULL, NULL,
                                             pkey) == 1) {
                        ok = EVP_DigestVerify(ctx, sig_buf, sig_len,
                                              fw, got);
                    }
                    free(fw);
                } else {
                    EVP_MD_CTX_free(ctx);
                    EVP_PKEY_free(pkey);
                    fclose(ffp);
                    return E_NO_MEM;
                }
            }
        }
        fclose(ffp);
    } else {
        /* RSA / EC：流式 SHA256 摘要 + EVP_DigestVerifyFinal */
        FILE *ffp = fopen(fw_path, "rb");
        if (!ffp) {
            LOG_ERROR("ota: open firmware %s failed: %s",
                      fw_path, strerror(errno));
            EVP_MD_CTX_free(ctx);
            EVP_PKEY_free(pkey);
            return E_IO;
        }

        if (EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pkey) == 1) {
            unsigned char fbuf[OTA_HTTP_BUF_SIZE];
            size_t n;
            int upd_ok = 1;
            while ((n = fread(fbuf, 1, sizeof(fbuf), ffp)) > 0) {
                if (EVP_DigestVerifyUpdate(ctx, fbuf, n) != 1) {
                    upd_ok = 0;
                    break;
                }
            }
            if (upd_ok)
                ok = EVP_DigestVerifyFinal(ctx, sig_buf, sig_len);
        }
        fclose(ffp);
    }

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    ERR_clear_error();

    if (ok != 1) {
        LOG_ERROR("ota: signature verification FAILED (fw=%s sig=%s)",
                  fw_path, sig_path);
        return E_INVAL;
    }

    LOG_INFO("ota: signature verification ok (%s)",
             is_ed25519 ? "Ed25519" : "SHA256/RSA-or-EC");
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

    /* P1-3: 文件写在锁外（写不成宁可不推进），成功后回锁更新内存 */
    if (ota_write_slot_file(OTA_CURRENT_SLOT_FILE, slot_str) != E_OK) {
        return E_IO;
    }

    pthread_mutex_lock(&g_state_lock);
    g_active_slot = slot;
    pthread_mutex_unlock(&g_state_lock);
    LOG_INFO("ota: switched to slot %s", slot_str);
    return E_OK;
}

/* ─── OTA 指令 JSON 解析 ────────────────────────────────── */

/*
 * 严格 JSON 指令解析（P1-7/P1-8 修复，v1.2.9）：
 * 格式: {"cmd":"upgrade","version":"2.0.1","url":"http://...","checksum":"sha256:abc...","force":false}
 *
 * P1-7（payload 截断/越界读）：
 *   旧实现用 strstr 直接在 payload 上扫，依赖 payload 以 NUL 结尾；
 *   MQTT 消息长度由 payload_len 给出，libmosquitto 保证 NUL 结尾但
 *   该契约未显式检查，且遇 ',' 即截断字段、无转义处理、超长静默截断。
 *   新实现先把 [0, len) 拷贝到 NUL 终止的本地缓冲（len 上限
 *   OTA_JSON_BUF_SIZE，超长直接拒绝），再用 json_get_string 提取：
 *   键必须是对象成员（左侧为 '{' 或 ','）、值带完整转义检查、
 *   超长拒绝而非截断、裸控制字符拒绝。
 *
 * P1-8（cmd 子串误匹配）：
 *   旧实现 `strstr(json, "upgrade")`——"cmd":"upgradex" 甚至
 *   url 里恰好含 "upgrade" 字样都会被当作升级指令。
 *   新实现提取 cmd 字符串值后 strcmp 精确比较 "upgrade"。
 *
 * 调用方（ota_handle_message）持有 g_state_lock，本函数写
 * g_target_* 全局是安全的。
 */
static int ota_parse_upgrade_cmd(const char *json, int len)
{
    if (!json || len <= 0 || len >= OTA_JSON_BUF_SIZE) {
        LOG_ERROR("ota: invalid payload length %d (max %d)",
                  len, OTA_JSON_BUF_SIZE - 1);
        return E_INVAL;
    }

    /* P1-7: 显式拷贝到 NUL 终止缓冲，解析不依赖调用方内存契约 */
    char buf[OTA_JSON_BUF_SIZE];
    memcpy(buf, json, (size_t)len);
    buf[len] = '\0';
    size_t buf_len = (size_t)len;

    char cmd[16] = {0};
    char version[OTA_VERSION_MAX] = {0};
    char url[OTA_URL_MAX] = {0};
    char checksum[OTA_CHECKSUM_MAX] = {0};

    if (!json_get_string(buf, buf_len, "cmd", cmd, sizeof(cmd))) {
        LOG_ERROR("ota: missing or malformed \"cmd\" field");
        return E_INVAL;
    }

    /* P1-8: 精确比较，杜绝 "upgradex"/内嵌子串误匹配 */
    if (strcmp(cmd, "upgrade") != 0) {
        LOG_ERROR("ota: not an upgrade command (cmd=\"%s\")", cmd);
        return E_INVAL;
    }

    if (!json_get_string(buf, buf_len, "version",
                         version, sizeof(version))) {
        LOG_ERROR("ota: missing or malformed \"version\" field");
        return E_INVAL;
    }

    if (!json_get_string(buf, buf_len, "url", url, sizeof(url))) {
        LOG_ERROR("ota: missing or malformed \"url\" field");
        return E_INVAL;
    }

    /* checksum 可缺省（空值走 VERIFYING 关卡时必然 mismatch → FAILED，
     * fail-closed）；存在但超长/畸形则拒绝解析。 */
    if (!json_get_string(buf, buf_len, "checksum",
                         checksum, sizeof(checksum))) {
        LOG_ERROR("ota: malformed \"checksum\" field");
        return E_INVAL;
    }

    /* 校验必要字段 */
    if (version[0] == '\0' || url[0] == '\0') {
        LOG_ERROR("ota: upgrade cmd missing version or url");
        return E_INVAL;
    }

    /* 提交到全局目标字段（调用方持锁） */
    memset(g_target_version, 0, sizeof(g_target_version));
    memset(g_download_url, 0, sizeof(g_download_url));
    memset(g_expected_checksum, 0, sizeof(g_expected_checksum));
    snprintf(g_target_version, sizeof(g_target_version), "%s", version);
    snprintf(g_download_url, sizeof(g_download_url), "%s", url);
    snprintf(g_expected_checksum, sizeof(g_expected_checksum), "%s",
             checksum);

    LOG_INFO("ota parsed: version=%s url=%s checksum=%s",
             g_target_version, g_download_url,
             g_expected_checksum[0] ? g_expected_checksum : "(none)");

    return E_OK;
}
