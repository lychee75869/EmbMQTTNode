/*
 * modbus_master.h / modbus_master.c
 * Modbus 主站协议模块（RTU + TCP）
 * 基于 libmodbus，支持编译期可选（BUILD_WITH_MODBUS）
 *
 * 阶段二：工业协议接入 —— 南向多协议采集
 */
#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include "common.h"

/* 初始化 Modbus 连接（根据配置选择 RTU 或 TCP） */
int modbus_master_init(const struct modbus_config *cfg);

/* 轮询一次所有从站，结果填充到 data 数组，返回采集条数 */
int modbus_master_poll(struct sensor_data *data, int max_count);

/* 检查 Modbus 模块是否已连接 */
int modbus_master_is_connected(void);

/* 关闭 Modbus 连接，释放资源 */
void modbus_master_close(void);

#endif /* MODBUS_MASTER_H */