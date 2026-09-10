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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
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

    printf("\n=== ALL OTA tests PASSED ===\n");
    return 0;
}
