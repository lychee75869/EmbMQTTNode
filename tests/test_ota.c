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
 */

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
#include <openssl/evp.h>
#include "../src/common.h"
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
        "ota_boot_attempt_max = 5\n";

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

/*
 * v1.2.7 辅助：fork 一个一次性 HTTP 服务器（监听 127.0.0.1 临时端口），
 * 对第一个连接返回 "HTTP/1.0 200 OK" + body。端口号经管道回传父进程。
 * alarm(30) 兜底：父进程意外失败时子进程不会永久阻塞在 accept。
 */
static pid_t start_local_http_server(const char *body, int body_len,
                                     int *port_out)
{
    int fds[2];
    assert(pipe(fds) == 0);

    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* ── 子进程：一次性 HTTP 服务器 ── */
        close(fds[0]);
        alarm(30);

        int srv = socket(AF_INET, SOCK_STREAM, 0);
        assert(srv >= 0);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;              /* 内核分配临时端口 */

        assert(bind(srv, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        assert(listen(srv, 1) == 0);

        struct sockaddr_in real;
        socklen_t rlen = sizeof(real);
        assert(getsockname(srv, (struct sockaddr *)&real, &rlen) == 0);
        int port = ntohs(real.sin_port);

        /* 端口告知父进程 */
        char msg[32];
        int m = snprintf(msg, sizeof(msg), "%d\n", port);
        assert((int)write(fds[1], msg, (size_t)m) == m);
        close(fds[1]);

        /* 接受一次连接，读完请求头即应答 */
        int c = accept(srv, NULL, NULL);
        assert(c >= 0);

        char req[2048];
        int total = 0;
        while (total < (int)sizeof(req) - 1) {
            ssize_t n = read(c, req + total, sizeof(req) - 1 - (size_t)total);
            if (n <= 0)
                break;
            total += (int)n;
            req[total] = '\0';
            if (strstr(req, "\r\n\r\n"))
                break;
        }

        char hdr[256];
        int h = snprintf(hdr, sizeof(hdr),
                         "HTTP/1.0 200 OK\r\n"
                         "Content-Type: application/octet-stream\r\n"
                         "Content-Length: %d\r\n"
                         "Connection: close\r\n"
                         "\r\n", body_len);
        assert((int)write(c, hdr, (size_t)h) == h);

        int off = 0;
        while (off < body_len) {
            ssize_t w = write(c, body + off, (size_t)(body_len - off));
            assert(w > 0);
            off += (int)w;
        }
        shutdown(c, SHUT_WR);
        close(c);
        close(srv);
        _exit(0);
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

    int port = 0;
    pid_t srv = start_local_http_server(fw, (int)sizeof(fw), &port);
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

    /* 回收 HTTP 服务器子进程 */
    int status = 0;
    pid_t r = waitpid(srv, &status, 0);
    assert(r == srv);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

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

/* ═══════════════════════════════════════════════════════════ */

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

    printf("\n=== ALL OTA tests PASSED ===\n");
    return 0;
}
