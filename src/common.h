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

#define EMBMQTTNODE_VERSION "1.0.0"

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

/* TLS 配置 */
struct tls_config {
    int     enabled;              /* 0=关闭, 1=单向认证, 2=双向认证 */
    char    ca_file[256];         /* CA 证书路径 */
    char    cert_file[256];       /* 客户端证书路径（双向认证） */
    char    key_file[256];        /* 客户端私钥路径（双向认证） */
    char    username[64];         /* MQTT 用户名（可选） */
    char    password[64];         /* MQTT 密码（可选） */
};

/* 设备身份信息 */
struct device_info {
    char    hostname[64];         /* 主机名 */
    char    mac_addr[18];         /* MAC 地址 xx:xx:xx:xx:xx:xx */
    char    mac_short[13];        /* MAC 后 6 位，用于 client_id */
    char    kernel_ver[64];       /* 内核版本 */
    char    cpu_model[128];       /* CPU 型号 */
    int64_t total_mem_kb;         /* 总内存 KB */
};

/* 配置结构 */
struct node_config {
    char    broker_host[128];
    int     broker_port;
    char    topic[128];
    char    client_id[64];
    int     sample_interval_ms;
    char    sensor_type[32];

    /* 阶段一新增：TLS + 安全 */
    struct tls_config tls;
};

/* 简单日志宏 */
#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)

#endif /* COMMON_H */
