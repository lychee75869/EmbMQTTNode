/*
 * common.h
 * 全局公共头文件：错误码、数据结构、日志宏
 */
#ifndef COMMON_H
#define COMMON_H

/* 启用 POSIX 扩展（gethostname, usleep 等）*/
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

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

/* ─── Modbus 协议配置 ─────────────────────────────────────── */

#define MODBUS_DEVICE_MAX   8      /* 最多接入 8 个从站 */
#define MODBUS_REG_MAX      32     /* 每个从站最多 32 个寄存器映射 */

/* 单条寄存器映射 */
struct modbus_reg_map {
    int     slave_id;             /* 从站地址 1-247 */
    int     reg_addr;             /* 寄存器起始地址 */
    int     reg_count;            /* 连续寄存器数量 */
    int     func_code;            /* 功能码 3=读保持 4=读输入 */
    char    data_type[16];        /* int16 / uint16 / float32 / int32 */
    char    field_name[32];       /* 映射到 sensor_data 的字段名 */
    double  scale;                /* 比例因子 */
    double  offset;               /* 偏移量 */
};

/* Modbus 总配置 */
struct modbus_config {
    int     enabled;              /* 0=关闭 1=启用 */
    char    mode[8];              /* "rtu" 或 "tcp" */

    /* RTU 参数 */
    char    serial_port[64];
    int     baudrate;
    char    parity[2];
    int     data_bits;
    int     stop_bits;

    /* TCP 参数 */
    char    tcp_host[128];
    int     tcp_port;

    /* 轮询与映射 */
    int     poll_interval_ms;
    int     reg_count;
    struct  modbus_reg_map regs[MODBUS_REG_MAX];
};

/* ─── TLS 配置 ────────────────────────────────────────────── */

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

    /* 阶段二新增：Modbus 工业协议 */
    struct modbus_config modbus;
};

/* 简单日志宏 */
#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)

#endif /* COMMON_H */
