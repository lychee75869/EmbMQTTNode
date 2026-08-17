/*
 * sensor.h / sensor.c
 * 传感器抽象层：支持模拟数据、BMP280、SHT30 等
 */
#ifndef SENSOR_H
#define SENSOR_H

#include "common.h"

/* 传感器类型 */
#define SENSOR_TYPE_MOCK    "mock"
#define SENSOR_TYPE_BMP280  "bmp280"
#define SENSOR_TYPE_SHT30   "sht30"
#define SENSOR_TYPE_EX      "extreme"

/* 初始化传感器，type 由配置指定 */
int sensor_init(const char *type);

/* 读取一次传感器数据 */
int sensor_read(struct sensor_data *data);

/* 关闭传感器 */
void sensor_close(void);

#endif /* SENSOR_H */
