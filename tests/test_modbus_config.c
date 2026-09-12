/*
 * tests/test_modbus_config.c
 * Modbus 配置解析单元测试
 *
 * 覆盖:
 *   - modbus_enabled / modbus_mode / modbus_tcp_host 解析
 *   - 寄存器映射 modbus_reg_N 解析（slave_id/addr/type/field/scale/offset）
 *   - int16 / float32 数据类型
 *   - 基础配置（broker_host/port）保留
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/common.h"
#include "../src/config.h"

/* ═══════════════════════════════════════════════════════════ */

static const char *test_config_content =
    "broker_host = 192.168.1.100\n"
    "broker_port = 1883\n"
    "modbus_enabled = 1\n"
    "modbus_mode = tcp\n"
    "modbus_tcp_host = 10.0.0.50\n"
    "modbus_tcp_port = 1502\n"
    "modbus_poll_interval_ms = 3000\n"
    "modbus_reg_1 = 1,40001,1,3,int16,temperature,0.1,0\n"
    "modbus_reg_2 = 2,40002,1,3,int16,humidity,0.05,0\n"
    "modbus_reg_3 = 1,40003,2,3,float32,pressure,1.0,-1000.0\n";

int main(void)
{
    /* 写临时配置文件 */
    const char *tmp_path = "test_modbus_tmp.conf";
    FILE *fp = fopen(tmp_path, "w");
    assert(fp != NULL);
    fprintf(fp, "%s", test_config_content);
    fclose(fp);

    /* 加载配置 */
    struct node_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    int rc = config_load(tmp_path, &cfg);
    assert(rc == E_OK);

    /* 验证基础配置保留 */
    assert(strcmp(cfg.broker_host, "192.168.1.100") == 0);
    assert(cfg.broker_port == 1883);

    /* 验证 Modbus 配置 */
    assert(cfg.modbus.enabled == 1);
    assert(strcmp(cfg.modbus.mode, "tcp") == 0);
    assert(strcmp(cfg.modbus.tcp_host, "10.0.0.50") == 0);
    assert(cfg.modbus.tcp_port == 1502);
    assert(cfg.modbus.poll_interval_ms == 3000);

    /* 验证寄存器映射数量 */
    assert(cfg.modbus.reg_count == 3);

    /* 验证 reg[0] */
    assert(cfg.modbus.regs[0].slave_id == 1);
    assert(cfg.modbus.regs[0].reg_addr == 40001);
    assert(cfg.modbus.regs[0].reg_count == 1);
    assert(cfg.modbus.regs[0].func_code == 3);
    assert(strcmp(cfg.modbus.regs[0].data_type, "int16") == 0);
    assert(strcmp(cfg.modbus.regs[0].field_name, "temperature") == 0);
    assert(cfg.modbus.regs[0].scale == 0.1);
    assert(cfg.modbus.regs[0].offset == 0.0);

    /* 验证 reg[1] */
    assert(cfg.modbus.regs[1].slave_id == 2);
    assert(cfg.modbus.regs[1].reg_addr == 40002);
    assert(strcmp(cfg.modbus.regs[1].field_name, "humidity") == 0);
    assert(cfg.modbus.regs[1].scale == 0.05);

    /* 验证 reg[2] (float32) */
    assert(cfg.modbus.regs[2].slave_id == 1);
    assert(cfg.modbus.regs[2].reg_addr == 40003);
    assert(cfg.modbus.regs[2].reg_count == 2);
    assert(strcmp(cfg.modbus.regs[2].data_type, "float32") == 0);
    assert(strcmp(cfg.modbus.regs[2].field_name, "pressure") == 0);
    assert(cfg.modbus.regs[2].scale == 1.0);
    assert(cfg.modbus.regs[2].offset == -1000.0);

    /* 打印配置 */
    config_dump(&cfg);

    /* ═════════════════════════════════════════════════════════ */
    /* v1.2.9 P1-4：非法寄存器映射必须在 config_load 阶段被拒绝
     * （LOG_WARN + 丢弃条目，不计入 reg_count）。
     * 合法域：slave_id 1-247；reg_count 1-MODBUS_REG_MAX(32)；
     * func 3 → addr ∈ [40001, 50001-count]；func 4 → addr ∈ [30001, 40001-count]。
     */
    const char *bad_config_content =
        "modbus_enabled = 1\n"
        "modbus_reg_1 = 1,40001,1,3,int16,temperature,0.1,0\n"    /* 合法基线 func3 */
        "modbus_reg_2 = 0,40002,1,3,int16,temperature,0.1,0\n"    /* slave_id=0 */
        "modbus_reg_3 = 248,40003,1,3,int16,temperature,0.1,0\n"  /* slave_id=248 */
        "modbus_reg_4 = 1,39999,1,3,int16,temperature,0.1,0\n"    /* func3 addr<40001 */
        "modbus_reg_5 = 1,50001,1,3,int16,temperature,0.1,0\n"    /* func3 addr>50000 */
        "modbus_reg_6 = 1,29999,1,4,int16,temperature,0.1,0\n"    /* func4 addr<30001 */
        "modbus_reg_7 = 1,40001,1,4,int16,temperature,0.1,0\n"    /* func4 用 40001 */
        "modbus_reg_8 = 1,40001,0,3,int16,temperature,0.1,0\n"    /* count=0 */
        "modbus_reg_9 = 1,40001,33,3,int16,temperature,0.1,0\n"   /* count>32 */
        "modbus_reg_10 = 1,40001,1,5,int16,temperature,0.1,0\n"   /* func 5 不支持 */
        "modbus_reg_11 = 1,49995,10,3,int16,temperature,0.1,0\n"  /* 49995-40001+10>10000 */
        "modbus_reg_12 = 1,30001,1,4,int16,temperature,0.1,0\n";  /* 合法边界 func4 */

    const char *bad_path = "test_modbus_bad_tmp.conf";
    fp = fopen(bad_path, "w");
    assert(fp != NULL);
    fprintf(fp, "%s", bad_config_content);
    fclose(fp);

    struct node_config bad_cfg;
    memset(&bad_cfg, 0, sizeof(bad_cfg));
    rc = config_load(bad_path, &bad_cfg);
    assert(rc == E_OK);

    /* 12 条映射只有 2 条合法（reg_1 func3 基线 + reg_12 func4 边界） */
    assert(bad_cfg.modbus.reg_count == 2);
    printf("\nP1-4 invalid mappings rejected (reg_count=%d):\n",
           bad_cfg.modbus.reg_count);

    assert(bad_cfg.modbus.regs[0].reg_addr == 40001);
    assert(bad_cfg.modbus.regs[0].func_code == 3);
    printf("  legal func3 40001 kept:      PASS\n");

    assert(bad_cfg.modbus.regs[1].reg_addr == 30001);
    assert(bad_cfg.modbus.regs[1].func_code == 4);
    printf("  legal func4 30001 kept:      PASS\n");

    /* 被拒条目不得残留（reg_count 之后的内容未定义，只验证数量语义） */
    remove(bad_path);
    printf("P1-4 modbus validation test PASSED\n");

    /* 清理 */
    remove(tmp_path);
    printf("\nmodbus config test PASSED\n");
    return 0;
}