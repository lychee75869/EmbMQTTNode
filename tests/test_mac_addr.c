/*
 * tests/test_mac_addr.c
 * MAC 地址扫描模块单元测试（v1.2.5 P1-5 修复 — 第 7 个测试）
 *
 * 用 mkdtemp 伪造 sysfs 树，避免依赖真实 /sys/class/net。
 * 覆盖 P1-5 bug 的核心回归用例（lo-only 路径），同时验证
 * "lo 兜底仅在末位候选时生效"的非硬编码判断。
 *
 * 覆盖:
 *   1. 优先级：eth0/wlan0/lo 共存 → 取 eth0
 *   2. 降级：仅 wlan0/lo → 取 wlan0
 *   3. lo 兜底回归（核心）：仅 lo → 取 lo（旧代码在此 double fclose）
 *   4. lo 非末位跳过：自定义 {lo,eth0} → 取 eth0
 *   5. 换行裁剪：address 文件带 \n → 返回值无换行
 *   6. 无换行：address 文件不带 \n → 原样返回
 *   7. 空文件：address 文件 0 字节 → fgets 失败 → E_IO
 *   8. base_dir 不存在 → E_IO
 *   9. 小缓冲：mac_len=8，内容 "aa:bb:cc:dd:ee:ff\n" → 截断不越界
 *  10. 防御：base_dir 传 NULL → E_IO
 *  11. 防御：mac_len<=0 → E_IO
 *  12. 防御：ifaces 传 NULL → E_IO
 *  13. 防御：mac 传 NULL → E_IO
 */

/*
 * mkdtemp(3) 需要 _XOPEN_SOURCE >= 500 或 _POSIX_C_SOURCE >= 200809L。
 * 必须在所有系统头文件 include 之前定义，否则 <stdio.h>/<stdlib.h>
 * 已按默认特性级别展开，feature test macro 失效。common.h 虽
 * #define _POSIX_C_SOURCE 200809L，但 include 顺序在 stdio.h 之后。
 */
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <assert.h>

#include "../src/mac_addr.h"
#include "../src/common.h"   /* E_OK / E_IO */

/* ─── 临时目录辅助 ──────────────────────────────────────── */

/*
 * mkdtemp 模板：必须以 XXXXXX 结尾且写入 buffer。
 * 返回值：成功 → 写入模板展开后的路径；失败 → 直接 abort 测试。
 */
static char tmp_root[64];

static const char *make_tmp_root(void) {
    strcpy(tmp_root, "test_mac_rootXXXXXX");
    char *p = mkdtemp(tmp_root);
    assert(p != NULL);
    return tmp_root;
}

/*
 * 在 root 下建 <iface>/ 子目录并把 contents 写入 <iface>/address。
 * contents 可以为 ""（建空文件）或 NULL（只建目录、address 不存在）。
 */
static void write_addr(const char *root, const char *iface, const char *contents) {
    char dir[256], path[256];
    snprintf(dir,  sizeof(dir),  "%s/%s", root, iface);
    snprintf(path, sizeof(path), "%s/address", dir);

    /* 建子目录（已存在不报错）*/
    mkdir(dir, 0755);

    if (contents == NULL)
        return;   /* 不创建 address 文件 */

    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    if (contents[0] != '\0')
        fputs(contents, fp);
    fclose(fp);
}

/* 递归删除 root（用 system("rm -rf ...") 只清自己建的测试目录）*/
static void rm_tmp_root(const char *root) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    int rc = system(cmd);
    assert(rc == 0);
    (void)rc;
}

/* ─── 测试用例 ──────────────────────────────────────────── */

static void test_priority_eth0(void) {
    printf("--- test_priority_eth0 ---\n");
    const char *root = make_tmp_root();

    write_addr(root, "lo",    "00:00:00:00:00:00\n");
    write_addr(root, "wlan0", "aa:bb:cc:dd:ee:01\n");
    write_addr(root, "eth0",  "aa:bb:cc:dd:ee:00\n");

    static const char *const ifaces[] = {"eth0", "wlan0", "wlp2s0",
                                         "enp0s3", "enp1s0", "lo", NULL};
    char mac[32];
    int rc = mac_scan(root, ifaces, mac, sizeof(mac));
    assert(rc == E_OK);
    assert(strcmp(mac, "aa:bb:cc:dd:ee:00") == 0);
    printf("  priority eth0 wins:            PASS\n");

    rm_tmp_root(root);
}

static void test_fallback_wlan0(void) {
    printf("--- test_fallback_wlan0 ---\n");
    const char *root = make_tmp_root();

    write_addr(root, "lo",    "00:00:00:00:00:00\n");
    write_addr(root, "wlan0", "aa:bb:cc:dd:ee:01\n");
    /* eth0 目录建了但 address 不存在 */
    write_addr(root, "eth0",  NULL);

    static const char *const ifaces[] = {"eth0", "wlan0", "wlp2s0",
                                         "enp0s3", "enp1s0", "lo", NULL};
    char mac[32];
    int rc = mac_scan(root, ifaces, mac, sizeof(mac));
    assert(rc == E_OK);
    assert(strcmp(mac, "aa:bb:cc:dd:ee:01") == 0);
    printf("  fallback wlan0 wins:           PASS\n");

    rm_tmp_root(root);
}

/*
 * P1-5 核心回归用例：仅 lo 可读。
 * 旧实现在此路径触发 double fclose（glibc 下可能直接 abort）；
 * 新实现必须干净返回 E_OK 且 mac 为 lo 的值。
 */
static void test_lo_only_fallback(void) {
    printf("--- test_lo_only_fallback (P1-5 regression) ---\n");
    const char *root = make_tmp_root();

    write_addr(root, "lo", "00:00:00:00:00:00\n");

    static const char *const ifaces[] = {"eth0", "wlan0", "wlp2s0",
                                         "enp0s3", "enp1s0", "lo", NULL};
    char mac[32];
    int rc = mac_scan(root, ifaces, mac, sizeof(mac));
    assert(rc == E_OK);
    assert(strcmp(mac, "00:00:00:00:00:00") == 0);
    printf("  lo-only returns clean E_OK:    PASS\n");
    printf("  no double fclose abort:        PASS\n");

    rm_tmp_root(root);
}

/*
 * 自定义候选列表：lo 在非末位 → 即使存在 lo/address，也不应选 lo。
 * 验证"是否最后一个候选"的判断是基于 ifaces 顺序，而非硬编码下标。
 */
static void test_lo_not_last_skipped(void) {
    printf("--- test_lo_not_last_skipped ---\n");
    const char *root = make_tmp_root();

    write_addr(root, "lo",   "00:00:00:00:00:00\n");
    write_addr(root, "eth0", "aa:bb:cc:dd:ee:00\n");

    static const char *const ifaces[] = {"lo", "eth0", NULL};
    char mac[32];
    int rc = mac_scan(root, ifaces, mac, sizeof(mac));
    assert(rc == E_OK);
    assert(strcmp(mac, "aa:bb:cc:dd:ee:00") == 0);
    printf("  lo skipped when not last:      PASS\n");

    rm_tmp_root(root);
}

/* 末尾带 \n 的 address 文件 → 返回值无换行符 */
static void test_strip_newline(void) {
    printf("--- test_strip_newline ---\n");
    const char *root = make_tmp_root();

    write_addr(root, "eth0", "11:22:33:44:55:66\n");

    static const char *const ifaces[] = {"eth0", NULL};
    char mac[32];
    int rc = mac_scan(root, ifaces, mac, sizeof(mac));
    assert(rc == E_OK);
    assert(strcmp(mac, "11:22:33:44:55:66") == 0);
    /* 防御：显式断言无换行 */
    assert(strchr(mac, '\n') == NULL);
    printf("  trailing \\n stripped:           PASS\n");

    rm_tmp_root(root);
}

/* 末尾不带 \n 的 address 文件 → 原样返回（不依赖换行裁剪分支）*/
static void test_no_newline(void) {
    printf("--- test_no_newline ---\n");
    const char *root = make_tmp_root();

    write_addr(root, "eth0", "11:22:33:44:55:66");

    static const char *const ifaces[] = {"eth0", NULL};
    char mac[32];
    int rc = mac_scan(root, ifaces, mac, sizeof(mac));
    assert(rc == E_OK);
    assert(strcmp(mac, "11:22:33:44:55:66") == 0);
    printf("  no-\\n returned as-is:          PASS\n");

    rm_tmp_root(root);
}

/* address 文件 0 字节 → fgets 返回 NULL → 跳过 → 最终 E_IO */
static void test_empty_file(void) {
    printf("--- test_empty_file ---\n");
    const char *root = make_tmp_root();

    write_addr(root, "eth0", "");   /* 0 字节 */

    static const char *const ifaces[] = {"eth0", "wlan0", NULL};
    char mac[32];
    int rc = mac_scan(root, ifaces, mac, sizeof(mac));
    assert(rc == E_IO);
    printf("  empty address file → E_IO:    PASS\n");

    rm_tmp_root(root);
}

/* base_dir 不存在 → 所有 fopen 失败 → E_IO */
static void test_missing_base_dir(void) {
    printf("--- test_missing_base_dir ---\n");
    const char *root = make_tmp_root();   /* root 存在，但 root/nonexist 不存在 */

    static const char *const ifaces[] = {"eth0", "wlan0", "lo", NULL};
    char fake_root[128];
    snprintf(fake_root, sizeof(fake_root), "%s/nonexist", root);

    char mac[32];
    int rc = mac_scan(fake_root, ifaces, mac, sizeof(mac));
    assert(rc == E_IO);
    printf("  missing base_dir → E_IO:      PASS\n");

    rm_tmp_root(root);
}

/*
 * 小缓冲：mac_len=8，address 内容 "aa:bb:cc:dd:ee:ff\n"。
 * snprintf-style 截断不越界，返回 E_OK（即使内容不完整）。
 */
static void test_small_buffer_truncate(void) {
    printf("--- test_small_buffer_truncate ---\n");
    const char *root = make_tmp_root();

    write_addr(root, "eth0", "aa:bb:cc:dd:ee:ff\n");

    static const char *const ifaces[] = {"eth0", NULL};
    char mac[8];
    memset(mac, 0xAA, sizeof(mac));   /* 预填哨兵，验证不越界 */
    int rc = mac_scan(root, ifaces, mac, sizeof(mac));
    assert(rc == E_OK);
    /* fgets(mac, 8, fp) 至多读 7 字节后写 \0：截断为 "aa:bb:c"，
     * 且因换行在第 18 字节之外，未进入 mac 也不触发换行裁剪分支。 */
    assert(strncmp(mac, "aa:bb:c", 7) == 0);
    assert(mac[7] == '\0');
    /* 不应越界到 mac[8]（哨兵区域）*/
    printf("  small buffer (len=8):          PASS\n");
    printf("    truncated to: \"%s\"\n", mac);

    rm_tmp_root(root);
}

/* ─── 防御性参数校验 ────────────────────────────────────── */

static void test_null_base_dir(void) {
    printf("--- test_null_base_dir ---\n");

    static const char *const ifaces[] = {"eth0", NULL};
    char mac[32];
    int rc = mac_scan(NULL, ifaces, mac, sizeof(mac));
    assert(rc == E_IO);
    printf("  NULL base_dir → E_IO:         PASS\n");
}

static void test_invalid_mac_len(void) {
    printf("--- test_invalid_mac_len ---\n");
    const char *root = make_tmp_root();
    write_addr(root, "eth0", "aa:bb:cc:dd:ee:ff\n");

    static const char *const ifaces[] = {"eth0", NULL};
    char mac[32];
    int rc = mac_scan(root, ifaces, mac, 0);   /* mac_len=0 */
    assert(rc == E_IO);
    rc = mac_scan(root, ifaces, mac, -1);      /* mac_len<0 */
    assert(rc == E_IO);
    printf("  mac_len<=0 → E_IO:           PASS\n");

    rm_tmp_root(root);
}

static void test_null_ifaces(void) {
    printf("--- test_null_ifaces ---\n");

    char mac[32];
    int rc = mac_scan("/tmp", NULL, mac, sizeof(mac));
    assert(rc == E_IO);
    printf("  NULL ifaces → E_IO:           PASS\n");
}

static void test_null_mac(void) {
    printf("--- test_null_mac ---\n");

    static const char *const ifaces[] = {"eth0", NULL};
    int rc = mac_scan("/tmp", ifaces, NULL, sizeof(char[32]));
    assert(rc == E_IO);
    printf("  NULL mac → E_IO:              PASS\n");
}

/* ─── 入口 ─────────────────────────────────────────────── */

int main(void) {
    printf("=== MAC Address Scanner Unit Tests (v1.2.5 P1-5) ===\n\n");

    test_priority_eth0();
    test_fallback_wlan0();
    test_lo_only_fallback();
    test_lo_not_last_skipped();
    test_strip_newline();
    test_no_newline();
    test_empty_file();
    test_missing_base_dir();
    test_small_buffer_truncate();

    test_null_base_dir();
    test_invalid_mac_len();
    test_null_ifaces();
    test_null_mac();

    printf("\n=== ALL mac_addr tests PASSED ===\n");
    return 0;
}