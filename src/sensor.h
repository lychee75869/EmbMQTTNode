/*
 * sensor.h / sensor.c
 * 传感器抽象层：支持模拟数据、SHT30、ADS1115 等
 */
#ifndef SENSOR_H
#define SENSOR_H

#include "common.h"

/* 传感器类型 */
#define SENSOR_TYPE_MOCK    "mock"
#define SENSOR_TYPE_SHT30   "sht30"
#define SENSOR_TYPE_ADS1115 "ads1115"

/*
 * 初始化传感器，type 由配置指定。
 * i2c_dev 为 I2C 适配器设备路径（如 /dev/i2c-1），
 * 仅在 type 为真实硬件（sht30/ads1115）时使用，mock 模式可传 NULL。
 */
int sensor_init(const char *type, const char *i2c_dev);

/* 读取一次传感器数据 */
int sensor_read(struct sensor_data *data);

/* 关闭传感器 */
void sensor_close(void);

#endif /* SENSOR_H */
