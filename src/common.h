/*
 * common.h
 * 全局公共头文件：错误码、数据结构、日志宏
 */
#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

/* 返回码 */
#define E_OK            0
#define E_INVAL        -1
#define E_NO_MEM       -2
#define E_IO           -3
#define E_NET          -4
#define E_TIMEOUT      -5
#define E_NOT_FOUND    -6

/* 传感器数据结构 */
struct sensor_data {
    double    temperature;   /* 摄氏度 */
    double    humidity;      /* %RH，无传感器时为 -1 */
    double    pressure;      /* hPa，无传感器时为 -1 */
    int64_t   timestamp_ms;  /* 毫秒时间戳 */
};

/* 配置结构 */
struct node_config {
    char  broker_host[128];
    int   broker_port;
    char  topic[128];
    char  client_id[64];
    int   sample_interval_ms;
    char  sensor_type[32];
};

/* 简单日志宏 */
#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#endif /* COMMON_H */
