/*
 * tests/test_modbus_config.c
 * Modbus 配置解析单元测试
 *
 * 验证 config_load 能正确解析 modbus_* 配置项和寄存器映射。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/common.h"
#include "../src/config.h"

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

    /* 清理 */
    remove(tmp_path);
    printf("\nmodbus config test PASSED\n");
    return 0;
}