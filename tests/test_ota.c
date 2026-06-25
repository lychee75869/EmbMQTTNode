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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
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

    printf("\n=== ALL OTA tests PASSED ===\n");
    return 0;
}
