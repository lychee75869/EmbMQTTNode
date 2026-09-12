/*
 * tests/test_ota.c
 * OTA 模块单元测试（阶段四）
 *
 * 覆盖:
 *   - ota_init / ota_close
 *   - ota_handle_message（JSON 解析）
 *   - ota_state_string
 *   - OTA 配置解析（via config_load）
 *   - 目录结构创建
 *   - 边界条件（NULL args, 禁用状态）
 *
 * v1.2.6 P0-3 修复后的状态机用例：
 *   - fresh boot（boot_count=0）→ 确认已健康
 *   - 试用递增（boot_count=1）→ 自增到 2
 *   - 达上限回滚（fork：post_boot_check exit(42)）
 *   - 健康确认清零（diff > boot_confirm_sec）
 *   - 未到确认时间不清零
 *   - 原子写无残留（目录扫描无 .tmp 文件）
 *
 * v1.2.7 fail-safe 用例（健康指示器持久化失败不得静默绕过回滚保护）：
 *   A. confirm 持久化失败 → 不清内存标志，下次调用自动重试
 *   B. 回滚切槽失败 → 不清零 count、不切槽，仍 exit(42) 等待下轮重试
 *   B2. 回滚切槽成功但清零失败 → 只告警，count 保持（告警路径）
 *   C. INSTALLING 写 boot_count="1" 失败 → 中止安装 state=FAILED，
 *      不切槽不重启（本地一次性 HTTP 服务器走通下载/校验/安装全链路）
 *   D. ota_write_slot_file 内部 rename 失败 → 补 LOG_ERROR 告警
 *      （fflush/fsync 失败分支无法在 tmpfs 稳定注入，未单测覆盖，
 *       由 src/ota.c 内注释说明、日志兜底）
 *
 * v1.2.9（P0-5 固件签名 + HTTPS + P1-14 响应头解析）新增用例：
 *   E. 验签函数独立单测：有效签名通过 / 篡改固件拒绝 / 换密钥拒绝 /
 *      .sig 缺失拒绝 / 公钥路径未配置或不可读拒绝
 *   F. fail-closed：公钥未配置 → 升级指令直接拒绝（不降级 checksum-only）
 *   G. http 全链路（响应头分两次 send 的 P1-14 路径）：有效签名 → 安装切槽
 *   H. 签名不匹配（checksum 过、签名不过）→ FAILED 不切槽
 *   I. .sig 404 → FAILED 不切槽
 *   J. https 全链路（自签证书当 CA，IP SAN）→ 安装切槽
 *   K. https 错误 CA → 下载失败 FAILED
 *
 * 兼容性说明：v1.2.6/v1.2.7 既有用例的断言零改动；仅测试脚手架随
 * fail-closed 语义适配（make_v127_config 预置公钥路径；用例 C 的
 * 一次性 HTTP 服务器升级为可服务 固件+.sig 的测试服务器）。
 */

/* 先引 common.h：拿到 _POSIX_C_SOURCE/_DEFAULT_SOURCE 定义，
 * 保证后续系统头暴露 usleep 等扩展声明 */
#include "../src/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include "../src/config.h"
#include "../src/ota.h"

static char g_test_dir[256];

/* 辅助: 创建测试用临时目录 */
static void setup_test_dir(void)
{
    snprintf(g_test_dir, sizeof(g_test_dir),
             "/tmp/embmqttnode_test_%d", (int)getpid());
    /* 清理旧测试数据 */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_test_dir);
    system(cmd);
    mkdir(g_test_dir, 0755);
}

static void cleanup_test_dir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_test_dir);
    system(cmd);
}

/*
 * v1.2.9 辅助：把测试公钥路径写入 cfg->public_key（fail-closed 要求
 * 该字段非空，否则升级指令被直接拒绝）。经 512 字节中转缓冲构造并
 * 断言不截断，避免 -Wformat-truncation 告警。
 */
static void set_test_pubkey(struct ota_config *cfg)
{
    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp), "%s/ota_pub.pem", g_test_dir);
    assert(n > 0 && (size_t)n < sizeof(cfg->public_key));
    memcpy(cfg->public_key, tmp, (size_t)n + 1);
}

/* ═══════════════════════════════════════════════════════════
 * 测试 1: 基本 init/close
 * ═══════════════════════════════════════════════════════════ */
static void test_init_close(void)
{
    printf("--- test_init_close ---\n");

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;

    /* 正常初始化 */
    assert(ota_init(&cfg, "test-client", "1.0.0") == E_OK);
    printf("  init ok:                PASS\n");

    /* 检查目录是否创建 */
    char path[512];
    snprintf(path, sizeof(path), "%s/slot_a", g_test_dir);
    struct stat st;
    assert(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    printf("  slot_a dir created:     PASS\n");

    snprintf(path, sizeof(path), "%s/slot_b", g_test_dir);
    assert(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    printf("  slot_b dir created:     PASS\n");

    snprintf(path, sizeof(path), "%s/download", g_test_dir);
    assert(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    printf("  download dir created:   PASS\n");

    /* 状态字符串 */
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  state is idle:          PASS\n");

    ota_close();

    /* NULL 参数 */
    assert(ota_init(NULL, "test", "1.0") == E_INVAL);
    assert(ota_init(&cfg, NULL, "1.0") == E_INVAL);
    assert(ota_init(&cfg, "test", NULL) == E_INVAL);
    printf("  NULL args rejected:     PASS\n");

    cleanup_test_dir();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 2: OTA 禁用状态
 * ═══════════════════════════════════════════════════════════ */
static void test_disabled(void)
{
    printf("--- test_disabled ---\n");

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 0;  /* 禁用 */
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;

    assert(ota_init(&cfg, "test-client", "1.0.0") == E_OK);

    /* 禁用时忽略消息 */
    const char *json = "{\"cmd\":\"upgrade\",\"version\":\"2.0.0\","
                       "\"url\":\"http://example.com/fw.bin\"}";
    ota_handle_message(json, (int)strlen(json));
    /* 禁用状态下 check_and_handle 返回 0 */
    assert(ota_check_and_handle() == 0);
    printf("  disabled: messages ignored: PASS\n");

    ota_close();
    cleanup_test_dir();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 3: MQTT 发布回调
 * ═══════════════════════════════════════════════════════════ */
static int g_test_publish_called = 0;
static char g_test_publish_topic[256];
static char g_test_publish_payload[512];

static int test_publish_cb(const char *topic, const char *payload, int qos)
{
    (void)qos;
    g_test_publish_called++;
    strncpy(g_test_publish_topic, topic, sizeof(g_test_publish_topic) - 1);
    strncpy(g_test_publish_payload, payload, sizeof(g_test_publish_payload) - 1);
    return 0;
}

static void test_publish_callback(void)
{
    printf("--- test_publish_callback ---\n");

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;
    /* v1.2.9 fail-closed：公钥路径必须非空，否则升级指令被直接拒绝 */
    set_test_pubkey(&cfg);

    assert(ota_init(&cfg, "test-client", "1.0.0") == E_OK);

    /* 注册回调 */
    ota_set_mqtt_publish(test_publish_cb);

    /* 发送有效的升级指令（HTTP 下载会失败但会触发状态上报） */
    const char *json =
        "{\"cmd\":\"upgrade\","
        "\"version\":\"2.0.1\","
        "\"url\":\"http://127.0.0.1:1/fw.bin\","
        "\"checksum\":\"sha256:abc\"}";

    g_test_publish_called = 0;
    ota_handle_message(json, (int)strlen(json));

    /* 进入 downloading 状态，然后 check_and_handle 会触发下载（失败） */
    /* 状态上报会通过回调发送 */
    assert(strcmp(ota_state_string(), "downloading") == 0);
    printf("  state transitions to downloading: PASS\n");

    /* 驱动状态机（HTTP 下载会失败 → 进入 failed → reset 到 idle） */
    ota_check_and_handle();  /* DOWNLOADING: 尝试 HTTP 下载 → 失败 → FAILED */
    assert(strcmp(ota_state_string(), "failed") == 0);
    printf("  download fails → failed state: PASS\n");

    /* 再次驱动 → reset to IDLE */
    ota_check_and_handle();
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  failed → idle reset:    PASS\n");

    /* 验证回调被调用 */
    assert(g_test_publish_called > 0);
    printf("  publish callback called: PASS\n");

    ota_close();
    cleanup_test_dir();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 4: OTA 指令 JSON 解析
 * ═══════════════════════════════════════════════════════════ */
static void test_json_parsing(void)
{
    printf("--- test_json_parsing ---\n");

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;
    /* v1.2.9 fail-closed：公钥路径必须非空（仅进 downloading 用） */
    set_test_pubkey(&cfg);

    assert(ota_init(&cfg, "test-client", "1.0.0") == E_OK);

    /* 有效 JSON */
    const char *valid =
        "{\"cmd\":\"upgrade\","
        "\"version\":\"3.0.0\","
        "\"url\":\"http://192.168.1.1:8080/fw.bin\","
        "\"checksum\":\"sha256:abcdef1234567890\"}";
    ota_handle_message(valid, (int)strlen(valid));
    assert(strcmp(ota_state_string(), "downloading") == 0);
    printf("  valid JSON accepted:    PASS\n");

    /* 每次只处理一个命令（状态非 IDLE 时忽略） */
    ota_close();

    /* 重新初始化测试无效 JSON */
    assert(ota_init(&cfg, "test-client", "1.0.0") == E_OK);

    /* 无效 JSON（缺少 version） — 注意我们的极简解析器可能不会严格拒绝 */
    const char *no_version =
        "{\"cmd\":\"upgrade\",\"url\":\"http://example.com/fw.bin\"}";
    ota_handle_message(no_version, (int)strlen(no_version));
    /* 解析失败后应该保持在 IDLE */
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  missing version rejected: PASS\n");

    /* 非 upgrade 命令 */
    const char *other_cmd = "{\"cmd\":\"status\"}";
    ota_handle_message(other_cmd, (int)strlen(other_cmd));
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  non-upgrade cmd skipped: PASS\n");

    ota_close();
    cleanup_test_dir();
}

/* ═══════════════════════════════════════════════════════════
 * 测试 5: OTA 配置解析（通过 config_load）
 * ═══════════════════════════════════════════════════════════ */
static void test_config_parsing(void)
{
    printf("--- test_config_parsing ---\n");

    const char *tmp_path = "/tmp/test_ota_tmp.conf";
    const char *content =
        "broker_host = 10.0.0.1\n"
        "broker_port = 1883\n"
        "ota_enabled = 1\n"
        "ota_slot_dir = /mnt/ota/test\n"
        "ota_boot_attempt_max = 5\n"
        "ota_public_key = /etc/embmqttnode/ota_pub.pem\n"
        "ota_ca_file = /etc/embmqttnode/ota_ca.pem\n";

    FILE *fp = fopen(tmp_path, "w");
    assert(fp);
    fprintf(fp, "%s", content);
    fclose(fp);

    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    assert(config_load(tmp_path, &cfg) == E_OK);

    assert(cfg.ota.enabled == 1);
    assert(strcmp(cfg.ota.slot_dir, "/mnt/ota/test") == 0);
    assert(cfg.ota.boot_attempt_max == 5);
    assert(strcmp(cfg.ota.public_key,
                  "/etc/embmqttnode/ota_pub.pem") == 0);
    assert(strcmp(cfg.ota.ca_file,
                  "/etc/embmqttnode/ota_ca.pem") == 0);
    printf("  ota config parsed ok:   PASS\n");

    remove(tmp_path);
}

/* ═══════════════════════════════════════════════════════════
 * v1.2.6 P0-3 修复——启动回滚 / 健康确认 状态机测试
 * ═══════════════════════════════════════════════════════════ */

/*
 * 在 g_test_dir 下写一个槽位文件，模拟上一轮 OTA 或 boot_count 写入的结果。
 * 用于：post_boot_check / ota_confirm_boot 等需要预置 slot_dir 状态的场景。
 */
static void write_slot_file(const char *name, const char *content)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_test_dir, name);
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    fputs(content, fp);
    fclose(fp);
}

/* 读取 slot_dir/<name> 内容到 buf，返回字节数；文件不存在返回 -1 */
static int read_slot_file(const char *name, char *buf, int buflen)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_test_dir, name);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int n = (int)fread(buf, 1, buflen - 1, fp);
    fclose(fp);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

/*
 * 用例 1（fresh boot）：boot_count 文件写 "0" → ota_init + post_boot_check
 * 不应有任何写动作，文件保持 "0"，状态机未触发回滚。
 */
static void test_fresh_boot_confirmed(void)
{
    printf("--- test_fresh_boot_confirmed ---\n");

    /* 预置 fresh state：slot_dir + boot_count=0 + current_slot=A */
    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "0\n");

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;
    cfg.boot_confirm_sec = 300;

    assert(ota_init(&cfg, "test-client", "1.2.6") == E_OK);
    ota_post_boot_check();
    ota_close();

    /* boot_count 仍为 "0" */
    char buf[16] = {0};
    int n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strchr(buf, '0') != NULL);
    assert(strspn(buf, "0\n") >= 1);
    printf("  fresh boot leaves counter at 0: PASS\n");

    /* current_slot 仍为 A */
    char slot[8] = {0};
    n = read_slot_file("current_slot", slot, sizeof(slot));
    assert(n > 0);
    assert(slot[0] == 'A');
    printf("  no slot switch on fresh boot:   PASS\n");

    cleanup_test_dir();
}

/*
 * 用例 2（试用递增）：boot_count=1, max=3 → post_boot_check 自增到 2，写文件
 */
static void test_trial_increment(void)
{
    printf("--- test_trial_increment ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "1\n");

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;
    cfg.boot_confirm_sec = 300;

    assert(ota_init(&cfg, "test-client", "1.2.6") == E_OK);
    ota_post_boot_check();
    ota_close();

    char buf[16] = {0};
    int n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    /* 文件里应该有 "2" */
    assert(strstr(buf, "2") != NULL);
    printf("  boot 1/3 incremented to 2:      PASS\n");
    printf("    file content: \"%s\"\n", buf);

    cleanup_test_dir();
}

/*
 * 用例 3（达上限回滚 — fork）：boot_count=3, max=3, current_slot=A → 子进程
 * 调 post_boot_check → exit(42)。父进程 waitpid 验证退出码、current_slot=B、
 * boot_count=0。回滚路径会 exit()，必须 fork 隔离。
 */
static void test_rollback_at_max(void)
{
    printf("--- test_rollback_at_max (fork) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "3\n");

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;
    cfg.boot_confirm_sec = 300;

    /* 父进程先 init 一次（共享 slot_dir 配置；fork 后子进程用同一组 OTA 全局）*/
    assert(ota_init(&cfg, "test-client", "1.2.6") == E_OK);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* 子进程：执行回滚路径（最终 exit(42)）；不要再做 ota_close */
        ota_post_boot_check();
        /* 不应到达这里 */
        _exit(99);
    }

    /* 父进程：等待子进程退出 */
    int status = 0;
    pid_t r = waitpid(pid, &status, 0);
    assert(r == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 42);
    printf("  child exited with status 42:    PASS\n");

    /* 子进程已 exit，无需父进程 ota_close（globals 一致，但保险起见再 close 一次） */
    ota_close();

    /* 验证文件状态：current_slot 已切到 B，boot_count 已清零 */
    char buf[16] = {0};
    int n = read_slot_file("current_slot", buf, sizeof(buf));
    assert(n > 0);
    assert(buf[0] == 'B');
    printf("  slot rolled back A→B:           PASS\n");

    n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strspn(buf, "0\n") >= 1);
    printf("  boot_count cleared by rollback: PASS\n");

    cleanup_test_dir();
}

/*
 * 用例 4（健康确认清零）：boot_count=1, boot_confirm_sec=1 → ota_init 后 sleep(2)
 * 调 ota_confirm_boot → boot_count 写到 "0"；再调一次（已 0） → no-op 不崩。
 */
static void test_confirm_clears_counter(void)
{
    printf("--- test_confirm_clears_counter ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "1\n");

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;
    cfg.boot_confirm_sec = 1;     /* 1 秒即确认，加速测试 */

    assert(ota_init(&cfg, "test-client", "1.2.6") == E_OK);

    /* 等待稳定运行 > boot_confirm_sec */
    sleep(2);
    ota_confirm_boot();

    char buf[16] = {0};
    int n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strspn(buf, "0\n") >= 1);
    printf("  confirm after %ds cleared counter: PASS\n", 2);

    /* 再调用一次（count 已 0）→ 应 no-op 不崩 */
    ota_confirm_boot();
    n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strspn(buf, "0\n") >= 1);
    printf("  second confirm is no-op:        PASS\n");

    ota_close();
    cleanup_test_dir();
}

/*
 * 用例 5（未到确认时间不清零）：boot_confirm_sec=3600，count=1
 * ota_init 后立刻 confirm → 文件保持 "1"
 */
static void test_confirm_too_early_noop(void)
{
    printf("--- test_confirm_too_early_noop ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "1\n");

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;
    cfg.boot_confirm_sec = 3600;  /* 远大于本测试 wall-clock */

    assert(ota_init(&cfg, "test-client", "1.2.6") == E_OK);

    /* 立即调用 confirm：time diff < 3600 → no-op */
    ota_confirm_boot();

    char buf[16] = {0};
    int n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "1") != NULL);
    printf("  confirm before timeout no-op:   PASS\n");
    printf("    file content: \"%s\"\n", buf);

    ota_close();
    cleanup_test_dir();
}

/*
 * 用例 6（原子写无残留）：连续触发多次 slot 文件写（switch + post_boot_check
 * 自增）后，扫描目录不应有 `*.tmp` 残留。
 */
static void test_no_tmp_residue(void)
{
    printf("--- test_no_tmp_residue ---\n");

    setup_test_dir();

    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;
    cfg.boot_confirm_sec = 300;

    assert(ota_init(&cfg, "test-client", "1.2.6") == E_OK);

    /* 触发若干次 slot 文件写：反复切换槽位、post_boot_check 递增计数等 */
    for (int i = 0; i < 4; i++) {
        /* 切到 B 再切回 A，每次都是新的原子写 */
        write_slot_file("boot_count", "1\n");   /* 让 post_boot_check 触发自增 */
        ota_post_boot_check();                  /* boot_count: 1 → 2 */
    }
    /* 关闭 */
    ota_close();

    /* 扫描目录，断言无 `*.tmp` 残留（隐藏文件以 . 开头，size > 0 后缀 .tmp）*/
    DIR *d = opendir(g_test_dir);
    assert(d != NULL);
    struct dirent *de;
    int residue = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        size_t nlen = strlen(de->d_name);
        if (nlen > 4 && strcmp(de->d_name + nlen - 4, ".tmp") == 0) {
            printf("    RESIDUE: %s\n", de->d_name);
            residue++;
        }
    }
    closedir(d);
    assert(residue == 0);
    printf("  no .tmp residue in slot_dir:    PASS\n");

    cleanup_test_dir();
}

/* ═══════════════════════════════════════════════════════════
 * v1.2.7 fail-safe 测试
 *
 * 注入持久化失败的手法说明：把 ota_write_slot_file 原子写的临时文件
 * 路径 <slot_dir>/.<name>.tmp 占成一个目录，fopen(tmp, "w") 将以
 * EISDIR 失败 → ota_write_slot_file 返回 E_IO。
 * 不用 chmod 555 方案的原因：WSL/CI 环境常以 root 跑测试，root 会
 * 绕过目录权限位，chmod 555 造不出写失败；tmp 路径目录占用对任何
 * 用户（含 root）都稳定复现。
 * ═══════════════════════════════════════════════════════════ */

/* v1.2.7 辅助：注入 ota_write_slot_file(name, ...) 持久化失败 */
static void inject_write_fail(const char *name)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", g_test_dir, name);
    assert(mkdir(tmp, 0755) == 0);
}

/* v1.2.7 辅助：解除注入，恢复正常写入 */
static void clear_write_fail(const char *name)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s/.%s.tmp", g_test_dir, name);
    rmdir(tmp);
}

/* v1.2.7 辅助：构造标准 OTA 配置（enabled, slot_dir=g_test_dir, max=3） */
static void make_v127_config(struct ota_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 1;
    strncpy(cfg->slot_dir, g_test_dir, sizeof(cfg->slot_dir) - 1);
    cfg->boot_attempt_max = 3;
    cfg->boot_confirm_sec = 300;
    /* v1.2.9 fail-closed：公钥路径必须非空，否则升级指令被直接拒绝。
     * 公钥文件按需由各用例生成到该路径；未触达 VERIFYING 的用例
     * （回滚/健康确认等）不需要文件真实存在。 */
    set_test_pubkey(cfg);
}

/*
 * v1.2.7 fail-safe 用例 A（缺陷 ⑤）：confirm 持久化失败时不清内存
 * 标志，主循环 5s 后的下一次调用自动重试。
 *
 * g_boot_attempt 是 ota.c 内部静态变量不可直接观测，用文件状态间接
 * 证明：若实现错误地在写失败后清了内存标志，第二次 confirm 会因
 * count==0 短路 no-op，文件永远停在 "1"；正确实现保留标志，恢复可写
 * 后第二次调用会真正把文件写成 "0"。
 */
static void test_confirm_retry_on_persist_fail(void)
{
    printf("--- test_confirm_retry_on_persist_fail (v1.2.7 A) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "1\n");

    struct ota_config cfg;
    make_v127_config(&cfg);
    cfg.boot_confirm_sec = 1;    /* 1 秒即确认，加速测试 */

    assert(ota_init(&cfg, "test-client", "1.2.7") == E_OK);

    /* 注入写失败：boot_count 原子写的 tmp 路径被目录占用 */
    inject_write_fail("boot_count");

    sleep(2);                    /* 越过 boot_confirm_sec */
    ota_confirm_boot();          /* 写失败 → LOG_ERROR + return，内存标志保留 */

    char buf[16] = {0};
    int n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "1") != NULL);
    printf("  write fail keeps file at 1:  PASS\n");

    /* 恢复可写，模拟主循环 5s 后的下一次 confirm 调用 */
    clear_write_fail("boot_count");
    ota_confirm_boot();

    n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strspn(buf, "0\n") >= 1);
    printf("  retry clears file to 0:     PASS\n");

    ota_close();
    cleanup_test_dir();
}

/*
 * v1.2.7 fail-safe 用例 B（缺陷 ②）：回滚路径切槽失败 → 不写
 * count=0、不切槽，仍 exit(42)（下轮重启重试回滚，StartLimitBurst 兜底）。
 * 验证：退出码 42、current_slot 仍为 A、boot_count 仍为 "3"。
 */
static void test_rollback_switch_fail_keeps_count(void)
{
    printf("--- test_rollback_switch_fail_keeps_count (v1.2.7 B) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "3\n");

    /* 注入切槽失败：current_slot 原子写的 tmp 路径被目录占用
     * → ota_switch_slot 返回 E_IO */
    inject_write_fail("current_slot");

    struct ota_config cfg;
    make_v127_config(&cfg);

    assert(ota_init(&cfg, "test-client", "1.2.7") == E_OK);

    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* 子进程：回滚路径 → 切槽失败 → exit(42) */
        ota_post_boot_check();
        _exit(99);   /* 不应到达 */
    }

    int status = 0;
    pid_t r = waitpid(pid, &status, 0);
    assert(r == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 42);
    printf("  child still exits 42:        PASS\n");

    ota_close();

    char buf[16] = {0};
    int n = read_slot_file("current_slot", buf, sizeof(buf));
    assert(n > 0);
    assert(buf[0] == 'A');
    printf("  slot not switched (A):       PASS\n");

    n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "3") != NULL);
    printf("  boot_count kept at 3:        PASS\n");

    clear_write_fail("current_slot");
    cleanup_test_dir();
}

/*
 * v1.2.7 fail-safe 用例 B2（缺陷 ③）：回滚切槽成功但 boot_count 清零
 * 失败 → 只 LOG_ERROR 告警（不阻断），照常 exit(42)。
 * 验证：退出码 42、current_slot 已切到 B、boot_count 保持 "3"
 * （下轮启动会再次触发回滚——乒乓风险由 StartLimitBurst 兜底，
 * 这是对齐过的安全失败方向）。
 */
static void test_rollback_clear_fail_keeps_count(void)
{
    printf("--- test_rollback_clear_fail_keeps_count (v1.2.7 B2) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "3\n");

    /* 只注入 boot_count 写失败，切槽写入不受影响 */
    inject_write_fail("boot_count");

    struct ota_config cfg;
    make_v127_config(&cfg);

    assert(ota_init(&cfg, "test-client", "1.2.7") == E_OK);

    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* 子进程：切槽成功 → 清零失败（只告警）→ exit(42) */
        ota_post_boot_check();
        _exit(99);   /* 不应到达 */
    }

    int status = 0;
    pid_t r = waitpid(pid, &status, 0);
    assert(r == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 42);
    printf("  child still exits 42:        PASS\n");

    ota_close();

    char buf[16] = {0};
    int n = read_slot_file("current_slot", buf, sizeof(buf));
    assert(n > 0);
    assert(buf[0] == 'B');
    printf("  slot switched to B:          PASS\n");

    n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "3") != NULL);
    printf("  boot_count kept at 3:        PASS\n");

    clear_write_fail("boot_count");
    cleanup_test_dir();
}

/*
 * v1.2.7 辅助：计算 SHA256 十六进制串（与 src/ota.c 的
 * ota_sha256_file 相同的小写 hex 输出，用于伪造升级指令的 checksum）。
 */
static void test_sha256_hex(const void *data, size_t len,
                            char *out, int outsz)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    assert(ctx != NULL);
    assert(EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1);
    assert(EVP_DigestUpdate(ctx, data, len) == 1);

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    assert(EVP_DigestFinal_ex(ctx, md, &md_len) == 1);
    EVP_MD_CTX_free(ctx);

    int off = 0;
    for (unsigned int i = 0; i < md_len && off < outsz - 1; i++) {
        off += snprintf(out + off, outsz - off, "%02x", md[i]);
    }
    out[off] = '\0';
}

/* ── v1.2.9 测试辅助：文件 / 密钥 / 证书 / 测试服务器 ───────── */

/* 写原始字节到文件 */
static void write_file_bytes(const char *path, const void *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    assert(fwrite(data, 1, len, fp) == len);
    fclose(fp);
}

/* 生成确定模式的伪固件内容 */
static void fill_fw(char *fw, size_t len)
{
    for (size_t i = 0; i < len; i++)
        fw[i] = (char)(i * 7 + 3);
}

/* 生成 RSA-2048 密钥对；pubkey_out_path 非 NULL 时导出公钥 PEM。
 * 返回 EVP_PKEY（调用方 EVP_PKEY_free）。 */
static EVP_PKEY *test_gen_rsa_key(const char *pubkey_out_path)
{
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    assert(pctx != NULL);
    assert(EVP_PKEY_keygen_init(pctx) == 1);
    assert(EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) == 1);
    EVP_PKEY *pkey = NULL;
    assert(EVP_PKEY_keygen(pctx, &pkey) == 1 && pkey != NULL);
    EVP_PKEY_CTX_free(pctx);

    if (pubkey_out_path) {
        BIO *bio = BIO_new_file(pubkey_out_path, "w");
        assert(bio != NULL);
        assert(PEM_write_bio_PUBKEY(bio, pkey) == 1);
        BIO_free(bio);
    }
    return pkey;
}

/* 对数据做 SHA256 签名（与设备侧 ota_verify_signature 的 RSA 路径对应） */
static void test_sign_data(EVP_PKEY *pkey, const void *data, size_t len,
                           unsigned char *sig, size_t *sig_len)
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    assert(ctx != NULL);
    assert(EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey) == 1);
    assert(EVP_DigestSignUpdate(ctx, data, len) == 1);

    size_t need = 0;
    assert(EVP_DigestSignFinal(ctx, NULL, &need) == 1);
    assert(need <= *sig_len);
    assert(EVP_DigestSignFinal(ctx, sig, &need) == 1);
    *sig_len = need;
    EVP_MD_CTX_free(ctx);
}

/* 用 openssl CLI 生成自签证书（含 IP SAN 127.0.0.1），既是服务端证书
 * 也是测试用 CA（与设备侧 X509_check_ip_asc 校验路径配套） */
static void gen_self_signed_cert(const char *cert_path, const char *key_path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -nodes -days 1 "
             "-subj /CN=127.0.0.1 "
             "-addext subjectAltName=IP:127.0.0.1 "
             "-keyout '%s' -out '%s' >/dev/null 2>&1",
             key_path, cert_path);
    int rc = system(cmd);
    assert(rc != -1);
    struct stat st;
    assert(stat(cert_path, &st) == 0 && st.st_size > 0);
}

/* 测试服务器连接发送（TLS / 裸 socket 自适应），返回已发送字节数 */
static int srv_conn_send(SSL *ssl, int fd, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        ssize_t n;
        if (ssl) {
            n = SSL_write(ssl, buf + off, len - off);
        } else {
            n = write(fd, buf + off, (size_t)(len - off));
        }
        if (n <= 0)
            return -1;
        off += (int)n;
    }
    return len;
}

/*
 * v1.2.9 辅助：fork 一个一次性测试服务器（监听 127.0.0.1 临时端口），
 * 替代 v1.2.7 的单请求 start_local_http_server（fail-closed 后设备
 * 每次升级要连两次：固件 + .sig）。
 *
 *   use_tls       1 = HTTPS（tls_cert/tls_key 为 PEM 路径）
 *   fw/fw_len     固件请求的 200 响应体
 *   sig/sig_len   ".sig" 请求的 200 响应体；sig==NULL 时 .sig 请求回 404
 *   split_header  1 = 响应头拆两次 send（半截头 + 剩余头，P1-14 用例）
 *
 * 服务器循环处理多个连接，直到父进程 SIGTERM 回收（alarm 兜底）。
 * 端口号经管道回传父进程。
 */
static pid_t start_ota_test_server(int use_tls,
                                   const char *tls_cert, const char *tls_key,
                                   const void *fw, int fw_len,
                                   const void *sig, int sig_len,
                                   int split_header, int *port_out)
{
    int fds[2];
    assert(pipe(fds) == 0);

    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* ── 子进程：一次性 HTTP(S) 服务器 ── */
        close(fds[0]);
        alarm(60);
        signal(SIGPIPE, SIG_IGN);

        int srv = socket(AF_INET, SOCK_STREAM, 0);
        assert(srv >= 0);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;              /* 内核分配临时端口 */

        int one = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        assert(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(listen(srv, 4) == 0);

        struct sockaddr_in real;
        socklen_t rlen = sizeof(real);
        assert(getsockname(srv, (struct sockaddr *)&real, &rlen) == 0);
        int port = ntohs(real.sin_port);

        /* 端口告知父进程 */
        char msg[32];
        int m = snprintf(msg, sizeof(msg), "%d\n", port);
        assert((int)write(fds[1], msg, (size_t)m) == m);
        close(fds[1]);

        /* 循环服务多个连接（固件 + .sig 各一次请求） */
        for (;;) {
            int c = accept(srv, NULL, NULL);
            if (c < 0)
                continue;

            SSL     *ssl  = NULL;
            SSL_CTX *sctx = NULL;
            if (use_tls) {
                sctx = SSL_CTX_new(TLS_server_method());
                assert(sctx != NULL);
                assert(SSL_CTX_use_certificate_file(
                           sctx, tls_cert, SSL_FILETYPE_PEM) == 1);
                assert(SSL_CTX_use_PrivateKey_file(
                           sctx, tls_key, SSL_FILETYPE_PEM) == 1);
                ssl = SSL_new(sctx);
                assert(ssl != NULL);
                SSL_set_fd(ssl, c);
                if (SSL_accept(ssl) != 1) {
                    SSL_free(ssl);
                    SSL_CTX_free(sctx);
                    close(c);
                    continue;
                }
            }

            /* 读完请求头（判断请求的是固件还是 .sig） */
            char req[2048] = {0};
            int total = 0;
            while (total < (int)sizeof(req) - 1) {
                ssize_t n;
                if (ssl) {
                    n = SSL_read(ssl, req + total,
                                 sizeof(req) - 1 - (size_t)total);
                } else {
                    n = read(c, req + total,
                             sizeof(req) - 1 - (size_t)total);
                }
                if (n <= 0)
                    break;
                total += (int)n;
                req[total] = '\0';
                if (strstr(req, "\r\n\r\n"))
                    break;
            }

            int    is_sig   = (strstr(req, ".sig") != NULL);
            int    code     = 200;
            /* fw/sig 为二进制（签名字节），统一按 const char * 处理 */
            const char *body     = is_sig ? (const char *)sig
                                          : (const char *)fw;
            int         body_len = is_sig ? sig_len : fw_len;
            if (is_sig && (!sig || sig_len <= 0)) {
                /* 用例 I：签名文件缺失 → 404 */
                code     = 404;
                body     = "not found";
                body_len = 9;
            }

            char hdr[256];
            int h = snprintf(hdr, sizeof(hdr),
                             "HTTP/1.0 %d %s\r\n"
                             "Content-Type: application/octet-stream\r\n"
                             "Content-Length: %d\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             code, code == 200 ? "OK" : "Not Found",
                             body_len);

            if (split_header) {
                /* P1-14 用例：半截头一次 send，间隔后发剩余头 + body */
                int half = h / 2;
                assert(srv_conn_send(ssl, c, hdr, half) == half);
                usleep(20000);
                assert(srv_conn_send(ssl, c, hdr + half, h - half)
                       == h - half);
            } else {
                assert(srv_conn_send(ssl, c, hdr, h) == h);
            }

            int off = 0;
            while (off < body_len) {
                int w = srv_conn_send(ssl, c, body + off, body_len - off);
                assert(w > 0);
                off += w;
            }

            if (ssl) {
                SSL_shutdown(ssl);
                SSL_free(ssl);
            }
            if (sctx)
                SSL_CTX_free(sctx);
            close(c);
        }
        _exit(0);   /* 不可达 */
    }

    /* ── 父进程：读取端口号 ── */
    close(fds[1]);
    char msg[32] = {0};
    ssize_t r = read(fds[0], msg, sizeof(msg) - 1);
    assert(r > 0);
    close(fds[0]);

    *port_out = atoi(msg);
    assert(*port_out > 0);
    return pid;
}

/* SIGTERM 回收测试服务器子进程 */
static void stop_test_server(pid_t pid)
{
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
}

/*
 * v1.2.7 fail-safe 用例 C（缺陷 ①）：INSTALLING 阶段写 boot_count="1"
 * 失败 → 中止安装（LOG_ERROR + 上报 error + state=FAILED），不切槽、
 * 不 exit(42)，留在当前好固件上等待重试。
 *
 * 通过本地一次性 HTTP 服务器把 DOWNLOADING / VERIFYING / INSTALLING
 * 全链路真实走通（真实 HTTP 下载 + 真实 SHA256 校验 + 真实文件复制），
 * 仅在 boot_count 持久化处注入失败。
 */
static void test_install_persist_fail_aborts(void)
{
    printf("--- test_install_persist_fail_aborts (v1.2.7 C) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "0\n");   /* 当前固件已确认健康 */

    /* 伪造固件内容（模式数据）及其 SHA256 */
    char fw[1024];
    for (int i = 0; i < (int)sizeof(fw); i++)
        fw[i] = (char)(i * 7 + 3);
    char sha[128];
    test_sha256_hex(fw, sizeof(fw), sha, sizeof(sha));

    /* v1.2.9 fail-closed 脚手架适配：生成 RSA 密钥对并对固件签名，
     * 公钥写到 make_v127_config 约定的 <g_test_dir>/ota_pub.pem，
     * 测试服务器同时服务固件与 .sig（设备侧断言保持零改动） */
    char pubkey_path[512];
    snprintf(pubkey_path, sizeof(pubkey_path), "%s/ota_pub.pem", g_test_dir);
    EVP_PKEY *pkey = test_gen_rsa_key(pubkey_path);
    assert(pkey != NULL);

    unsigned char sig[512];
    size_t sig_len = sizeof(sig);
    test_sign_data(pkey, fw, sizeof(fw), sig, &sig_len);
    char sig_path[512];
    snprintf(sig_path, sizeof(sig_path), "%s/ota_fw_c.sig", g_test_dir);
    write_file_bytes(sig_path, sig, sig_len);

    int port = 0;
    pid_t srv = start_ota_test_server(0, NULL, NULL,
                                      fw, (int)sizeof(fw),
                                      sig, (int)sig_len, 0, &port);
    assert(port > 0);

    struct ota_config cfg;
    make_v127_config(&cfg);

    assert(ota_init(&cfg, "test-client", "1.2.7") == E_OK);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"upgrade\",\"version\":\"2.0.0\","
             "\"url\":\"http://127.0.0.1:%d/fw.bin\","
             "\"checksum\":\"sha256:%s\"}", port, sha);
    ota_handle_message(json, (int)strlen(json));
    assert(strcmp(ota_state_string(), "downloading") == 0);

    /* 注入 boot_count 写失败（INSTALLING 阶段才会触碰该文件） */
    inject_write_fail("boot_count");

    /* DOWNLOADING：本地 HTTP 下载成功 */
    int rc = ota_check_and_handle();
    assert(rc == 1);
    assert(strcmp(ota_state_string(), "verifying") == 0);
    printf("  download via local http:    PASS\n");

    /* VERIFYING：SHA256 匹配 */
    rc = ota_check_and_handle();
    assert(rc == 1);
    assert(strcmp(ota_state_string(), "installing") == 0);
    printf("  checksum verify ok:          PASS\n");

    /* INSTALLING：复制成功，但写 boot_count="1" 失败 → 中止 */
    rc = ota_check_and_handle();
    assert(rc == 1);
    assert(strcmp(ota_state_string(), "failed") == 0);
    printf("  persist fail aborts install: PASS\n");

    /* 安装中止：不切槽、不清计数、不 exit(42)（否则到不了这里） */
    char buf[16] = {0};
    int n = read_slot_file("current_slot", buf, sizeof(buf));
    assert(n > 0);
    assert(buf[0] == 'A');
    printf("  current_slot stays A:        PASS\n");

    n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strspn(buf, "0\n") >= 1);
    printf("  boot_count stays 0:          PASS\n");

    /* 固件本体已复制到备用槽（中止发生在复制之后、切槽之前） */
    char fwpath[512];
    struct stat st;
    snprintf(fwpath, sizeof(fwpath), "%s/slot_b/embmqttnode", g_test_dir);
    assert(stat(fwpath, &st) == 0);
    assert(st.st_size == (off_t)sizeof(fw));
    printf("  firmware copied to slot_b:   PASS\n");

    /* FAILED → 下一次驱动复位到 IDLE（v1.2.6 既有语义，可重试升级） */
    rc = ota_check_and_handle();
    assert(rc == 0);
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  failed resets to idle:       PASS\n");

    /* 回收测试服务器子进程（SIGTERM；循环服务型，不再期望自然退出） */
    stop_test_server(srv);

    EVP_PKEY_free(pkey);
    ota_close();
    clear_write_fail("boot_count");
    cleanup_test_dir();
}

/*
 * v1.2.7 fail-safe 用例 D（ota_write_slot_file 内部补日志）：rename
 * 失败分支的 LOG_ERROR 告警。注入手法：把 current_slot 本身占成目录 →
 * rename(.current_slot.tmp → current_slot) 以 EISDIR 失败。子进程把
 * stderr 重定向到文件，父进程断言错误日志确实落盘。
 * 注：fflush/fsync 失败分支在 tmpfs 上无法稳定注入，未单测覆盖，
 * 由 src/ota.c 内注释说明，运行时靠 LOG_ERROR 告警兜底。
 */
static void test_write_rename_fail_logs_error(void)
{
    printf("--- test_write_rename_fail_logs_error (v1.2.7 D) ---\n");

    setup_test_dir();
    write_slot_file("boot_count", "3\n");

    /* current_slot 占成目录：rename 的目标是目录 → EISDIR */
    char slotdir[512];
    snprintf(slotdir, sizeof(slotdir), "%s/current_slot", g_test_dir);
    assert(mkdir(slotdir, 0755) == 0);

    struct ota_config cfg;
    make_v127_config(&cfg);

    /* ota_init：current_slot 读失败（fgets 对目录返回 EISDIR）→ 默认 A，
     * 且初始化时补写 "A" 也会 rename 失败 → 印证该分支有日志（父进程
     * stderr 可见，此处不断言） */
    assert(ota_init(&cfg, "test-client", "1.2.7") == E_OK);

    char errfile[512];
    snprintf(errfile, sizeof(errfile), "%s/stderr_capture.txt", g_test_dir);

    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* 子进程：stderr 重定向到文件后走回滚路径
         * → 切槽 → rename 失败 → LOG_ERROR + exit(42) */
        FILE *ef = freopen(errfile, "w", stderr);
        assert(ef != NULL);
        ota_post_boot_check();
        _exit(99);   /* 不应到达 */
    }

    int status = 0;
    pid_t r = waitpid(pid, &status, 0);
    assert(r == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 42);
    printf("  child exits 42:              PASS\n");

    ota_close();

    /* 读取捕获的 stderr，断言 rename 失败有 LOG_ERROR 落盘 */
    FILE *fp = fopen(errfile, "r");
    assert(fp != NULL);
    char logbuf[4096];
    size_t got = fread(logbuf, 1, sizeof(logbuf) - 1, fp);
    fclose(fp);
    logbuf[got] = '\0';
    assert(strstr(logbuf, "rename") != NULL);
    assert(strstr(logbuf, "current_slot") != NULL);
    printf("  rename failure logged:       PASS\n");

    /* 切槽失败不清零：boot_count 保持 "3" */
    char buf[16] = {0};
    int n = read_slot_file("boot_count", buf, sizeof(buf));
    assert(n > 0);
    assert(strstr(buf, "3") != NULL);
    printf("  boot_count kept at 3:        PASS\n");

    /* current_slot 仍是目录（未被 rename 覆盖成文件） */
    struct stat st;
    assert(stat(slotdir, &st) == 0);
    assert(S_ISDIR(st.st_mode));
    printf("  current_slot not replaced:   PASS\n");

    cleanup_test_dir();
}

/* ═══════════════════════════════════════════════════════════
 * v1.2.9 测试（P0-5 固件签名 fail-closed + HTTPS + P1-14）
 * ═══════════════════════════════════════════════════════════ */

/*
 * 用例 E：ota_verify_signature 独立单测。
 * 覆盖：有效签名 / 篡改固件 / 换密钥 / .sig 缺失 / 公钥路径未配置 /
 * 公钥文件不可读。
 */
static void test_sig_verify_unit(void)
{
    printf("--- test_sig_verify_unit (v1.2.9 E) ---\n");

    setup_test_dir();

    char fw[512];
    fill_fw(fw, sizeof(fw));

    char fw_path[512], sig_path[512], pub_path[512];
    snprintf(fw_path,  sizeof(fw_path),  "%s/unit_fw.bin",  g_test_dir);
    snprintf(sig_path, sizeof(sig_path), "%s/unit_fw.sig",  g_test_dir);
    snprintf(pub_path, sizeof(pub_path), "%s/unit_pub.pem", g_test_dir);
    write_file_bytes(fw_path, fw, sizeof(fw));

    EVP_PKEY *pkey = test_gen_rsa_key(pub_path);
    assert(pkey != NULL);

    unsigned char sig[512];
    size_t sig_len = sizeof(sig);
    test_sign_data(pkey, fw, sizeof(fw), sig, &sig_len);
    write_file_bytes(sig_path, sig, sig_len);

    /* 1. 有效签名 → E_OK */
    assert(ota_verify_signature(fw_path, sig_path, pub_path) == E_OK);
    printf("  valid signature accepted:        PASS\n");

    /* 2. 篡改固件（1 字节）→ 拒绝 */
    char fw_tamper[512];
    snprintf(fw_tamper, sizeof(fw_tamper),
             "%s/unit_fw_tampered.bin", g_test_dir);
    fw[10] ^= 0xFF;
    write_file_bytes(fw_tamper, fw, sizeof(fw));
    fw[10] ^= 0xFF;
    assert(ota_verify_signature(fw_tamper, sig_path, pub_path) != E_OK);
    printf("  tampered firmware rejected:      PASS\n");

    /* 3. 换密钥（另一把私钥签的固件，配本钥匙公钥验）→ 拒绝 */
    char pub2_path[512], sig2_path[512];
    snprintf(pub2_path, sizeof(pub2_path), "%s/unit_pub2.pem", g_test_dir);
    snprintf(sig2_path, sizeof(sig2_path), "%s/unit_fw_other.sig", g_test_dir);
    EVP_PKEY *other = test_gen_rsa_key(pub2_path);
    assert(other != NULL);
    unsigned char sig2[512];
    size_t sig2_len = sizeof(sig2);
    test_sign_data(other, fw, sizeof(fw), sig2, &sig2_len);
    write_file_bytes(sig2_path, sig2, sig2_len);
    assert(ota_verify_signature(fw_path, sig2_path, pub_path) != E_OK);
    printf("  wrong-key signature rejected:    PASS\n");
    EVP_PKEY_free(other);

    /* 4. .sig 文件缺失 → 拒绝 */
    assert(ota_verify_signature(fw_path, "/nonexistent/fw.sig",
                                pub_path) != E_OK);
    printf("  missing sig file rejected:       PASS\n");

    /* 5. 公钥路径未配置（空串）→ 拒绝 */
    assert(ota_verify_signature(fw_path, sig_path, "") != E_OK);
    printf("  empty pubkey path rejected:      PASS\n");

    /* 6. 公钥文件不可读 → 拒绝 */
    assert(ota_verify_signature(fw_path, sig_path,
                                "/nonexistent/ota_pub.pem") != E_OK);
    printf("  unreadable pubkey rejected:      PASS\n");

    EVP_PKEY_free(pkey);
    cleanup_test_dir();
}

/*
 * 用例 F：fail-closed 核心语义——公钥未配置时升级指令直接拒绝，
 * 不进入 DOWNLOADING（绝不降级回 checksum-only）。
 */
static void test_pubkey_unconfigured_rejected(void)
{
    printf("--- test_pubkey_unconfigured_rejected (v1.2.9 F) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");

    struct ota_config cfg;
    make_v127_config(&cfg);
    cfg.public_key[0] = '\0';   /* 关键：未配置公钥 */

    assert(ota_init(&cfg, "test-client", "1.2.9") == E_OK);
    ota_set_mqtt_publish(test_publish_cb);

    const char *json =
        "{\"cmd\":\"upgrade\",\"version\":\"2.0.0\","
        "\"url\":\"http://127.0.0.1:1/fw.bin\","
        "\"checksum\":\"sha256:abc\"}";

    g_test_publish_called = 0;
    g_test_publish_payload[0] = '\0';
    ota_handle_message(json, (int)strlen(json));

    /* 指令被直接拒绝：不进入 downloading */
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  upgrade cmd rejected (idle):     PASS\n");

    /* 上报 error */
    assert(g_test_publish_called == 1);
    assert(strstr(g_test_publish_payload, "\"error\"") != NULL);
    printf("  error status reported:           PASS\n");

    /* 状态机推进 no-op（没有开始任何下载） */
    assert(ota_check_and_handle() == 0);
    printf("  no download attempted:           PASS\n");

    ota_close();
    cleanup_test_dir();
}

/*
 * 用例 G：http 全链路 + P1-14。
 * 服务器把响应头拆成两次 send（半截头 + 剩余头+body），设备侧用
 * 累积缓冲解析必须正确切分且不丢 body 字节（固件 SHA256 匹配 +
 * 槽内固件字节数完整即证明）。有效签名 → 安装切槽 → 试用计数。
 */
static void test_full_chain_signature_ok(void)
{
    printf("--- test_full_chain_signature_ok (v1.2.9 G) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");

    char fw[1024];
    fill_fw(fw, sizeof(fw));
    char sha[128];
    test_sha256_hex(fw, sizeof(fw), sha, sizeof(sha));

    char pubkey_path[512], sig_path[512];
    snprintf(pubkey_path, sizeof(pubkey_path),
             "%s/ota_pub.pem", g_test_dir);
    snprintf(sig_path, sizeof(sig_path), "%s/fw_ok.sig", g_test_dir);

    EVP_PKEY *pkey = test_gen_rsa_key(pubkey_path);
    assert(pkey != NULL);
    unsigned char sig[512];
    size_t sig_len = sizeof(sig);
    test_sign_data(pkey, fw, sizeof(fw), sig, &sig_len);
    write_file_bytes(sig_path, sig, sig_len);

    int port = 0;
    /* split_header=1：响应头分两次 send（P1-14 累积缓冲解析路径） */
    pid_t srv = start_ota_test_server(0, NULL, NULL,
                                      fw, (int)sizeof(fw),
                                      sig, (int)sig_len, 1, &port);

    struct ota_config cfg;
    make_v127_config(&cfg);
    assert(ota_init(&cfg, "test-client", "1.2.9") == E_OK);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"upgrade\",\"version\":\"3.0.0\","
             "\"url\":\"http://127.0.0.1:%d/fw.bin\","
             "\"checksum\":\"sha256:%s\"}", port, sha);

    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        /* 子进程：全链路 下载→校验→安装→切槽 → REBOOTING exit(42) */
        ota_handle_message(json, (int)strlen(json));
        while (ota_check_and_handle() == 1)
            ;
        _exit(99);   /* 不应到达 */
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 42);
    printf("  full chain upgrade, exit 42:     PASS\n");

    ota_close();

    /* 切槽成功：current_slot=B、boot_count=1（进入试用） */
    char buf[16] = {0};
    assert(read_slot_file("current_slot", buf, sizeof(buf)) > 0);
    assert(buf[0] == 'B');
    printf("  slot switched to B:              PASS\n");

    assert(read_slot_file("boot_count", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "1") != NULL);
    printf("  boot_count=1 (trial):            PASS\n");

    /* 固件字节完整落槽（分片头解析无丢字节） */
    char fwpath[512];
    struct stat st;
    snprintf(fwpath, sizeof(fwpath), "%s/slot_b/embmqttnode", g_test_dir);
    assert(stat(fwpath, &st) == 0);
    assert(st.st_size == (off_t)sizeof(fw));
    printf("  firmware intact in slot_b:       PASS\n");

    stop_test_server(srv);
    EVP_PKEY_free(pkey);
    cleanup_test_dir();
}

/*
 * 用例 H：签名不匹配——服务器下发的固件与被签名的固件内容不同，
 * 但 checksum 按下发固件计算（checksum 关通过），签名关必须拦截。
 * 验证 FAILED、不切槽、boot_count 不动、可复位重试。
 */
static void test_full_chain_sig_mismatch(void)
{
    printf("--- test_full_chain_sig_mismatch (v1.2.9 H) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "0\n");

    char fw_served[1024], fw_signed[1024];
    fill_fw(fw_served, sizeof(fw_served));
    fill_fw(fw_signed, sizeof(fw_signed));
    fw_served[100] = (char)(fw_served[100] + 1);   /* 仅差 1 字节 */

    /* checksum 按下发的 fw_served 计算 → checksum 关放行 */
    char sha[128];
    test_sha256_hex(fw_served, sizeof(fw_served), sha, sizeof(sha));

    char pubkey_path[512], sig_path[512];
    snprintf(pubkey_path, sizeof(pubkey_path),
             "%s/ota_pub.pem", g_test_dir);
    snprintf(sig_path, sizeof(sig_path), "%s/fw_mismatch.sig", g_test_dir);

    EVP_PKEY *pkey = test_gen_rsa_key(pubkey_path);
    assert(pkey != NULL);
    unsigned char sig[512];
    size_t sig_len = sizeof(sig);
    test_sign_data(pkey, fw_signed, sizeof(fw_signed), sig, &sig_len);
    write_file_bytes(sig_path, sig, sig_len);

    int port = 0;
    pid_t srv = start_ota_test_server(0, NULL, NULL,
                                      fw_served, (int)sizeof(fw_served),
                                      sig, (int)sig_len, 0, &port);

    struct ota_config cfg;
    make_v127_config(&cfg);
    assert(ota_init(&cfg, "test-client", "1.2.9") == E_OK);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"upgrade\",\"version\":\"9.9.9\","
             "\"url\":\"http://127.0.0.1:%d/fw.bin\","
             "\"checksum\":\"sha256:%s\"}", port, sha);
    ota_handle_message(json, (int)strlen(json));

    int rc = ota_check_and_handle();    /* DOWNLOADING：固件+.sig 均下载 */
    assert(rc == 1);
    assert(strcmp(ota_state_string(), "verifying") == 0);

    rc = ota_check_and_handle();        /* checksum 过、签名不过 → FAILED */
    assert(rc == 1);
    assert(strcmp(ota_state_string(), "failed") == 0);
    printf("  sig mismatch → FAILED:           PASS\n");

    /* 不切槽、不写 boot_count=1 */
    char buf[16] = {0};
    assert(read_slot_file("current_slot", buf, sizeof(buf)) > 0);
    assert(buf[0] == 'A');
    printf("  current_slot stays A:            PASS\n");

    assert(read_slot_file("boot_count", buf, sizeof(buf)) > 0);
    assert(strspn(buf, "0\n") >= 1);
    printf("  boot_count stays 0:              PASS\n");

    rc = ota_check_and_handle();        /* FAILED → IDLE（可重试） */
    assert(rc == 0);
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  failed resets to idle:           PASS\n");

    stop_test_server(srv);
    EVP_PKEY_free(pkey);
    ota_close();
    cleanup_test_dir();
}

/*
 * 用例 I：.sig 下载 404（中间人剥离签名 / 源站部署不全）→
 * fail-closed，下载阶段即 FAILED，不进入校验/安装。
 */
static void test_sig_download_missing(void)
{
    printf("--- test_sig_download_missing (v1.2.9 I) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "0\n");

    char fw[1024];
    fill_fw(fw, sizeof(fw));
    char sha[128];
    test_sha256_hex(fw, sizeof(fw), sha, sizeof(sha));

    char pubkey_path[512];
    snprintf(pubkey_path, sizeof(pubkey_path),
             "%s/ota_pub.pem", g_test_dir);
    EVP_PKEY *pkey = test_gen_rsa_key(pubkey_path);
    assert(pkey != NULL);

    int port = 0;
    /* sig=NULL → .sig 请求回 404 */
    pid_t srv = start_ota_test_server(0, NULL, NULL,
                                      fw, (int)sizeof(fw),
                                      NULL, 0, 0, &port);

    struct ota_config cfg;
    make_v127_config(&cfg);
    assert(ota_init(&cfg, "test-client", "1.2.9") == E_OK);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"upgrade\",\"version\":\"9.9.8\","
             "\"url\":\"http://127.0.0.1:%d/fw.bin\","
             "\"checksum\":\"sha256:%s\"}", port, sha);
    ota_handle_message(json, (int)strlen(json));

    int rc = ota_check_and_handle();    /* 固件下载 OK，.sig 404 → FAILED */
    assert(rc == 1);
    assert(strcmp(ota_state_string(), "failed") == 0);
    printf("  sig 404 → FAILED:                PASS\n");

    char buf[16] = {0};
    assert(read_slot_file("current_slot", buf, sizeof(buf)) > 0);
    assert(buf[0] == 'A');
    printf("  current_slot stays A:            PASS\n");

    assert(read_slot_file("boot_count", buf, sizeof(buf)) > 0);
    assert(strspn(buf, "0\n") >= 1);
    printf("  boot_count stays 0:              PASS\n");

    rc = ota_check_and_handle();
    assert(rc == 0);
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  failed resets to idle:           PASS\n");

    stop_test_server(srv);
    EVP_PKEY_free(pkey);
    ota_close();
    cleanup_test_dir();
}

/*
 * 用例 J：https 全链路。自签证书（IP SAN 127.0.0.1）既是服务端证书
 * 也是信任 CA（ota_ca_file 指向），TLS 握手 + 证书链 + 主机名/IP 校验
 * 全部通过 → 下载 → 验签 → 安装切槽（fork，exit 42）。
 */
static void test_https_full_chain(void)
{
    printf("--- test_https_full_chain (v1.2.9 J) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");

    /* 自签证书 + 私钥（openssl CLI 生成，含 IP SAN） */
    char cert[512], cakey[512];
    snprintf(cert,  sizeof(cert),  "%s/tls_cert.pem", g_test_dir);
    snprintf(cakey, sizeof(cakey), "%s/tls_key.pem",  g_test_dir);
    gen_self_signed_cert(cert, cakey);

    char fw[1024];
    fill_fw(fw, sizeof(fw));
    char sha[128];
    test_sha256_hex(fw, sizeof(fw), sha, sizeof(sha));

    char pubkey_path[512], sig_path[512];
    snprintf(pubkey_path, sizeof(pubkey_path),
             "%s/ota_pub.pem", g_test_dir);
    snprintf(sig_path, sizeof(sig_path), "%s/fw_https.sig", g_test_dir);

    EVP_PKEY *pkey = test_gen_rsa_key(pubkey_path);
    assert(pkey != NULL);
    unsigned char sig[512];
    size_t sig_len = sizeof(sig);
    test_sign_data(pkey, fw, sizeof(fw), sig, &sig_len);
    write_file_bytes(sig_path, sig, sig_len);

    int port = 0;
    pid_t srv = start_ota_test_server(1, cert, cakey,
                                      fw, (int)sizeof(fw),
                                      sig, (int)sig_len, 0, &port);

    struct ota_config cfg;
    make_v127_config(&cfg);
    /* 设备侧信任自签 CA（测试 CA 锚点手法，见任务说明） */
    strncpy(cfg.ca_file, cert, sizeof(cfg.ca_file) - 1);
    cfg.ca_file[sizeof(cfg.ca_file) - 1] = '\0';

    assert(ota_init(&cfg, "test-client", "1.2.9") == E_OK);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"upgrade\",\"version\":\"4.0.0\","
             "\"url\":\"https://127.0.0.1:%d/fw.bin\","
             "\"checksum\":\"sha256:%s\"}", port, sha);

    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        ota_handle_message(json, (int)strlen(json));
        while (ota_check_and_handle() == 1)
            ;
        _exit(99);   /* 不应到达 */
    }

    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 42);
    printf("  https full chain, exit 42:       PASS\n");

    ota_close();

    char buf[16] = {0};
    assert(read_slot_file("current_slot", buf, sizeof(buf)) > 0);
    assert(buf[0] == 'B');
    printf("  slot switched to B:              PASS\n");

    assert(read_slot_file("boot_count", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "1") != NULL);
    printf("  boot_count=1 (trial):            PASS\n");

    char fwpath[512];
    struct stat st;
    snprintf(fwpath, sizeof(fwpath), "%s/slot_b/embmqttnode", g_test_dir);
    assert(stat(fwpath, &st) == 0);
    assert(st.st_size == (off_t)sizeof(fw));
    printf("  firmware intact in slot_b:       PASS\n");

    stop_test_server(srv);
    EVP_PKEY_free(pkey);
    cleanup_test_dir();
}

/*
 * 用例 K：证书校验失败——设备信任的 CA 与服务端证书不匹配（错 CA），
 * TLS 握手必须失败（SSL_VERIFY_PEER），下载失败 → FAILED。
 */
static void test_https_bad_ca_rejected(void)
{
    printf("--- test_https_bad_ca_rejected (v1.2.9 K) ---\n");

    setup_test_dir();
    write_slot_file("current_slot", "A\n");
    write_slot_file("boot_count",   "0\n");

    /* 服务端证书（pair1）与设备信任的 CA（pair2）是两套独立密钥 */
    char cert[512], cakey[512], wrongca[512], wrongkey[512];
    snprintf(cert,     sizeof(cert),     "%s/tls_cert.pem", g_test_dir);
    snprintf(cakey,    sizeof(cakey),    "%s/tls_key.pem",  g_test_dir);
    snprintf(wrongca,  sizeof(wrongca),  "%s/wrong_ca.pem", g_test_dir);
    snprintf(wrongkey, sizeof(wrongkey), "%s/wrong_key.pem", g_test_dir);
    gen_self_signed_cert(cert, cakey);
    gen_self_signed_cert(wrongca, wrongkey);   /* 另一套独立密钥的错 CA */

    char fw[1024];
    fill_fw(fw, sizeof(fw));
    char sha[128];
    test_sha256_hex(fw, sizeof(fw), sha, sizeof(sha));

    char pubkey_path[512], sig_path[512];
    snprintf(pubkey_path, sizeof(pubkey_path),
             "%s/ota_pub.pem", g_test_dir);
    snprintf(sig_path, sizeof(sig_path), "%s/fw_badca.sig", g_test_dir);

    EVP_PKEY *pkey = test_gen_rsa_key(pubkey_path);
    assert(pkey != NULL);
    unsigned char sig[512];
    size_t sig_len = sizeof(sig);
    test_sign_data(pkey, fw, sizeof(fw), sig, &sig_len);
    write_file_bytes(sig_path, sig, sig_len);

    int port = 0;
    pid_t srv = start_ota_test_server(1, cert, cakey,
                                      fw, (int)sizeof(fw),
                                      sig, (int)sig_len, 0, &port);

    struct ota_config cfg;
    make_v127_config(&cfg);
    strncpy(cfg.ca_file, wrongca, sizeof(cfg.ca_file) - 1);
    cfg.ca_file[sizeof(cfg.ca_file) - 1] = '\0';   /* 错 CA → 校验必败 */

    assert(ota_init(&cfg, "test-client", "1.2.9") == E_OK);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"upgrade\",\"version\":\"4.0.1\","
             "\"url\":\"https://127.0.0.1:%d/fw.bin\","
             "\"checksum\":\"sha256:%s\"}", port, sha);
    ota_handle_message(json, (int)strlen(json));

    int rc = ota_check_and_handle();    /* TLS 证书校验失败 → 下载失败 */
    assert(rc == 1);
    assert(strcmp(ota_state_string(), "failed") == 0);
    printf("  wrong CA → download FAILED:      PASS\n");

    char buf[16] = {0};
    assert(read_slot_file("current_slot", buf, sizeof(buf)) > 0);
    assert(buf[0] == 'A');
    printf("  current_slot stays A:            PASS\n");

    rc = ota_check_and_handle();
    assert(rc == 0);
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  failed resets to idle:           PASS\n");

    stop_test_server(srv);
    EVP_PKEY_free(pkey);
    ota_close();
    cleanup_test_dir();
}

/* ═══════════════════════════════════════════════════════════ */

/*
 * v1.2.9 P1-7/P1-8 用例：
 *   L. json_get_string 纯函数单测（键位置/转义/截断拒绝/长度边界/
 *      控制字符/未闭合）
 *   M. ota_handle_message 严格解析：cmd="upgradex" 拒绝（P1-8 子串
 *      误匹配）、url 内嵌 "upgrade" 不误匹配、payload_len 截断拒绝
 *      （P1-7 不依赖 NUL 结尾、不越界读）、len 之后垃圾字节不影响
 */
static void test_p1_strict_parsing(void)
{
    printf("--- test_p1_strict_parsing (P1-7/P1-8) ---\n");

    char out[64];

    /* ── L: json_get_string 纯函数 ── */
    assert(json_get_string("{\"a\":\"b\"}", strlen("{\"a\":\"b\"}"),
                           "a", out, sizeof(out)) == 1);
    assert(strcmp(out, "b") == 0);
    printf("  basic member extraction:  PASS\n");

    assert(json_get_string("{\"a\":\"x\",\"b\":\"y\"}",
                           strlen("{\"a\":\"x\",\"b\":\"y\"}"),
                           "b", out, sizeof(out)) == 1);
    assert(strcmp(out, "y") == 0);
    printf("  second member extracted:  PASS\n");

    /* 键必须是完整成员名：前缀串 "myversion" 不得误命中 "version"
     * （P1-7 键位置检查：左侧最近非空白字符须为 '{' 或 ','） */
    assert(json_get_string("{\"myversion\":\"9\"}",
                           strlen("{\"myversion\":\"9\"}"),
                           "version", out, sizeof(out)) == 0);
    printf("  key prefix not matched:   PASS\n");

    /* 值超长：拒绝而非静默截断（P1-9 同源语义） */
    assert(json_get_string("{\"k\":\"0123456789012345678901234567890\"}",
                           strlen("{\"k\":\"0123456789012345678901234567890\"}"),
                           "k", out, 8) == 0);
    printf("  oversize value rejected:  PASS\n");

    /* 转义支持：\" \\ \/ */
    const char *esc = "{\"k\":\"a\\\"b\\\\c\"}";
    assert(json_get_string(esc, strlen(esc), "k", out, sizeof(out)) == 1);
    assert(strcmp(out, "a\"b\\c") == 0);
    printf("  escapes decoded:          PASS\n");

    /* 不支持的 \uXXXX：严格拒绝 */
    const char *uesc = "{\"k\":\"\\u0041\"}";
    assert(json_get_string(uesc, strlen(uesc), "k", out, sizeof(out)) == 0);
    printf("  \\uXXXX escape rejected:   PASS\n");

    /* 裸控制字符（原始换行）：拒绝 */
    const char *ctl = "{\"k\":\"a\nb\"}";
    assert(json_get_string(ctl, strlen(ctl), "k", out, sizeof(out)) == 0);
    printf("  raw control char rejected: PASS\n");

    /* 只在 [0, len) 扫描：len 之后有垃圾字节、且无 NUL 终止也不越界 */
    {
        char buf[32];
        memcpy(buf, "{\"a\":\"ok\"}", 9);
        memset(buf + 9, 'Z', 10);   /* 垃圾字节，故意不写 NUL */
        assert(json_get_string(buf, 9, "a", out, sizeof(out)) == 1);
        assert(strcmp(out, "ok") == 0);
    }
    printf("  bounded by len (no NUL):  PASS\n");

    /* 未闭合字符串值：拒绝 */
    const char *unclosed = "{\"a\":\"un closed";
    assert(json_get_string(unclosed, strlen(unclosed),
                           "a", out, sizeof(out)) == 0);
    printf("  unterminated value reject: PASS\n");

    /* ── M: ota_handle_message 严格解析 ── */
    struct ota_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.enabled = 1;
    strncpy(cfg.slot_dir, g_test_dir, sizeof(cfg.slot_dir) - 1);
    cfg.boot_attempt_max = 3;
    set_test_pubkey(&cfg);
    assert(ota_init(&cfg, "test-client", "1.0.0") == E_OK);

    /* P1-8: cmd="upgradex" 必须拒绝（旧 strstr(json,"upgrade") 会放行） */
    const char *cmd_prefix =
        "{\"cmd\":\"upgradex\",\"version\":\"2.0.1\","
        "\"url\":\"http://127.0.0.1:1/fw.bin\",\"checksum\":\"sha256:a\"}";
    ota_handle_message(cmd_prefix, (int)strlen(cmd_prefix));
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  cmd=upgradex rejected:    PASS\n");

    /* P1-8: cmd 为其他值、url 内嵌 "upgrade" 字样 → 拒绝
     * （旧实现两个 strstr 条件都命中，会被误当作升级指令） */
    const char *url_substr =
        "{\"cmd\":\"status\",\"version\":\"2.0.1\","
        "\"url\":\"http://127.0.0.1/upgrade.bin\","
        "\"checksum\":\"sha256:a\"}";
    ota_handle_message(url_substr, (int)strlen(url_substr));
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  upgrade-in-url rejected:  PASS\n");

    /* P1-7: payload_len 短于实际 JSON（末尾被截断、未闭合）→ 拒绝，
     * 且不得越界读 payload 缓冲 */
    const char *valid =
        "{\"cmd\":\"upgrade\",\"version\":\"2.0.1\","
        "\"url\":\"http://127.0.0.1:1/fw.bin\","
        "\"checksum\":\"sha256:abc\"}";
    ota_handle_message(valid, (int)strlen(valid) - 8);
    assert(strcmp(ota_state_string(), "idle") == 0);
    printf("  truncated payload reject: PASS\n");

    /* P1-7: len 之后的垃圾字节不影响有效指令（只扫 [0, len)） */
    {
        char buf[256];
        memcpy(buf, valid, strlen(valid));
        memset(buf + strlen(valid), 'X', 16);   /* 垃圾，不写 NUL */
        ota_handle_message(buf, (int)strlen(valid));
        assert(strcmp(ota_state_string(), "downloading") == 0);
    }
    printf("  trailing garbage ignored: PASS\n");

    ota_close();
    printf("  test_p1_strict_parsing: ALL PASS\n");
}

int main(void)
{
    printf("=== OTA Unit Tests ===\n\n");

    setup_test_dir();

    test_init_close();
    test_disabled();
    test_publish_callback();
    test_json_parsing();
    test_config_parsing();

    /* v1.2.6 P0-3 状态机用例 */
    test_fresh_boot_confirmed();
    test_trial_increment();
    test_rollback_at_max();
    test_confirm_clears_counter();
    test_confirm_too_early_noop();
    test_no_tmp_residue();

    /* v1.2.7 fail-safe 用例 */
    test_confirm_retry_on_persist_fail();
    test_rollback_switch_fail_keeps_count();
    test_rollback_clear_fail_keeps_count();
    test_install_persist_fail_aborts();
    test_write_rename_fail_logs_error();

    /* v1.2.9 固件签名 + HTTPS + P1-14 用例 */
    test_sig_verify_unit();
    test_pubkey_unconfigured_rejected();
    test_full_chain_signature_ok();
    test_full_chain_sig_mismatch();
    test_sig_download_missing();
    test_https_full_chain();
    test_https_bad_ca_rejected();

    /* v1.2.9 P1-7/P1-8 严格解析用例 */
    test_p1_strict_parsing();

    printf("\n=== ALL OTA tests PASSED ===\n");
    return 0;
}
