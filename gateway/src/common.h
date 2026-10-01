/*
 * common.h —— 全局公共头：错误码、数据结构、日志宏
 *
 * 不变式：platform=local 路径的行为与旧版本线的 local 路径**逐字节等价**
 * （各单元测试即其回归基线）。
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

#define EMBMQTTNODE_VERSION "0.1.0"

/* 返回码：0 成功，非 0 失败 */
#define E_OK            0 // 成功
#define E_INVAL        -1 //参数错误
#define E_NO_MEM       -2 //内存不足
#define E_IO           -3 //IO错误
#define E_NET          -4 //网络错误
#define E_TIMEOUT      -5 //超时
#define E_NOT_FOUND    -6 // 未找到

/*
 * 传感器数据来源。
 * SOURCE_LOCAL 的枚举值 0 必须保留 —— 旧数据库回放依赖它。
 */
typedef enum sensor_source {
    SOURCE_LOCAL  = 0, /* 本地板载传感器（枚举值保留，勿删） */
    SOURCE_MODBUS = 1, /* Modbus 从站数据 */
} sensor_source_t;

/*
 * 全项目唯一的"无此字段 / 无效值"哨兵。
 *
 * 语义：字段值 == SENSOR_VALUE_INVALID 一律按"无效值"处理——消费侧不参与判定，
 * huawei builder 侧整体省略该字段不上报（local 路径不 mask）。
 * 所有哨兵赋值点与判定点必须统一引用本宏：早期曾出现某传感器无测量时写 -1.0、
 * 而判定只拦 -999.0，导致无效值被当真实读数上报。
 */
#define SENSOR_VALUE_INVALID (-999.0)

/*
 * 传感器数据结构。
 * 无有效读数的字段一律 = SENSOR_VALUE_INVALID；生产侧（modbus_master）遍历字段表
 * 统一置哨兵（见 sensor_fields.h 表序不变式：新增字段只许追加表尾）。
 */
struct sensor_data {
    double    temperature;   /* 摄氏度 */
    double    humidity;      /* %RH，无有效读数时 = SENSOR_VALUE_INVALID */
    double    pressure;      /* hPa，无有效读数时 = SENSOR_VALUE_INVALID */
    double    soil_moisture; /* 土壤湿度 %，无有效读数时 = SENSOR_VALUE_INVALID */
    double    water_level;   /* 水槽水位 %，无有效读数时 = SENSOR_VALUE_INVALID */
    double    battery_voltage; /* 电池电压 V，无有效读数时 = SENSOR_VALUE_INVALID */
    int64_t   timestamp_ms;  /* 毫秒时间戳（采样时刻，事件 event_time 源） */
    int64_t   id;            /* SQLite 自增主键，0=未持久化（storage_save 成功后回填） */
    sensor_source_t source;  /* 数据来源（storage_get_pending 读出，补发时用于选 topic） */
    int       source_id;     /* 数据源实例标识（供子设备注册表路由）：
                              *   SOURCE_LOCAL  恒 0
                              *   SOURCE_MODBUS 取从站地址 slave_id 1..247
                              *   未来 BLE 等数据源另占位 */
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
    double  scale;
    double  offset;
};

struct modbus_config {
    int     enabled;              /* 0=关闭 1=启用 */
    char    mode[8];              /* "rtu" 或 "tcp" */

    char    serial_port[64];      /* RTU 参数 */
    int     baudrate;
    char    parity[2];
    int     data_bits;
    int     stop_bits;

    char    tcp_host[128];        /* TCP 参数 */
    int     tcp_port;

    int     poll_interval_ms;
    int     reg_count;
    struct  modbus_reg_map regs[MODBUS_REG_MAX];
};

/* ─── TLS 配置 ────────────────────────────────────────────── */

struct tls_config {
    int     enabled;              /* 0=关闭, 1=单向认证, 2=双向认证 */
    char    ca_file[256];
    char    cert_file[256];       /* 双向认证时使用 */
    char    key_file[256];        /* 双向认证时使用 */
    char    username[64];         /* MQTT 用户名（可选） */
    char    password[64];         /* MQTT 密码（可选） */
};

/* 设备身份信息 */
struct device_info {
    char    hostname[64];
    char    mac_addr[18];         /* xx:xx:xx:xx:xx:xx */
    char    mac_short[13];        /* MAC 后 6 位，用于 client_id */
    char    kernel_ver[64];
    char    cpu_model[128];
    int64_t total_mem_kb;
};

/*
 * 结构化告警事件：供平台层（huawei 物模型事件）消费。
 * local 路径只用 msg 字段，msg 文本须与历史行为逐字节一致。
 */
struct alert_event {
    char    rule_name[32];   /* 触发规则/异常名 */
    char    field[32];       /* 触发字段名 */
    double  value;           /* 触发时的字段值 */
    double  threshold;       /* 触发阈值（outside 取区间下界） */
    char    source_kind[16]; /* "sensor"（source_id==0）/ "modbus"（source_id!=0） */
    int     source_id;       /* 数据源实例：modbus slave_id / 本地传感器 0 */
    char    msg[256];        /* 引擎生成文本，须与历史行为相同 */
    int64_t ts_ms;           /* 采样时刻（事件 event_time 源） */
};

/* ─── 严格 JSON 字符串字段提取 ─────────────────────────────── */

/*
 * json_get_string - 严格按 JSON 语法提取字符串字段值。
 * 返回 1 = 找到并完整提取；0 = 未找到 / 格式非法 / 值过长（拒绝而非截断）。
 * 只在给定长度内扫描（不依赖 NUL 结尾）；key 必须处于键位置；值过长或转义
 * 不认识则拒绝而非截断（安全相关，勿放宽）。
 */
static inline int json_get_string(const char *json, size_t json_len,
                                  const char *key, char *out, size_t outsz)
{
    if (!json || !key || !out || outsz < 2)
        return 0;

    size_t klen = strlen(key);
    if (klen == 0 || klen > 64)
        return 0;

    /* 带引号的 key 字面量："key" */
    char kq[70];
    kq[0] = '"';
    memcpy(kq + 1, key, klen);
    kq[klen + 1] = '"';

    for (size_t i = 0; i + klen + 2 <= json_len; i++) {
        if (memcmp(json + i, kq, klen + 2) != 0)
            continue;

        /* 键位置检查：左侧最近的非空白字符必须是 '{' 或 ','，
         * 否则这是值/嵌套内容里的字样，不是键 */
        int at_key_pos = 0;
        for (size_t j = i; j > 0; ) {
            j--;
            char c = json[j];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                continue;
            at_key_pos = (c == '{' || c == ',');
            break;
        }
        if (!at_key_pos)
            continue;

        /* 跳过空白 → ':' → 跳过空白 → 开引号 */
        size_t p = i + klen + 2;
        while (p < json_len && (json[p] == ' '  || json[p] == '\t' ||
                                json[p] == '\n' || json[p] == '\r'))
            p++;
        if (p >= json_len || json[p] != ':')
            continue;
        p++;
        while (p < json_len && (json[p] == ' '  || json[p] == '\t' ||
                                json[p] == '\n' || json[p] == '\r'))
            p++;
        if (p >= json_len || json[p] != '"')
            continue;
        p++;    /* 进入值内容 */

        /* 逐字符解码，直到闭引号 */
        size_t o = 0;
        int closed = 0;
        while (p < json_len) {
            char c = json[p];
            if (c == '"') {
                closed = 1;
                break;
            }
            if (c == '\\') {
                if (p + 1 >= json_len)
                    return 0;   /* 转义悬空 */
                char decoded;
                switch (json[p + 1]) {
                case '"':  decoded = '"';  break;
                case '\\': decoded = '\\'; break;
                case '/':  decoded = '/';  break;
                case 'b':  decoded = '\b'; break;
                case 'f':  decoded = '\f'; break;
                case 'n':  decoded = '\n'; break;
                case 'r':  decoded = '\r'; break;
                case 't':  decoded = '\t'; break;
                default:
                    return 0;   /* \uXXXX 等：严格子集，不支持即拒绝 */
                }
                if (o + 1 >= outsz)
                    return 0;   /* 值过长：拒绝而非截断 */
                out[o++] = decoded;
                p += 2;
                continue;
            }
            if ((unsigned char)c < 0x20)
                return 0;       /* 裸控制字符：非法 JSON */
            if (o + 1 >= outsz)
                return 0;       /* 值过长：拒绝而非截断 */
            out[o++] = c;
            p++;
        }
        if (!closed)
            return 0;           /* 字符串未闭合 */

        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* ─── OTA 远程升级配置 ─────────────────────────────────────── */

#define OTA_SLOT_DIR_DEFAULT          "/var/lib/embmqttnode"
#define OTA_URL_MAX                   512
#define OTA_CHECKSUM_MAX              256
#define OTA_VERSION_MAX               64
#define OTA_BOOT_ATTEMPT_MAX          3
#define OTA_BOOT_CONFIRM_SEC_DEFAULT  300   /* 新固件稳定运行该秒数后确认本次启动健康 */

/* OTA 状态机 */
enum ota_state {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_INSTALLING,
    OTA_STATE_REBOOTING,
    OTA_STATE_FAILED,
};

/*
 * OTA 配置。签名与下载为 fail-closed（详见 ota.h）：
 * public_key 空串 = 未配置 → 升级指令直接拒绝（不降级到 checksum-only）；
 * 已配置则验签为硬性关卡，.sig 缺失 / 签名不匹配 / 公钥不可读一律拒绝安装。
 * ca_file / ca_path 控制 https 下载的证书校验锚点，两者均空时尝试系统 CA
 * 常见位置，找不到即拒绝 https 下载。
 */
struct ota_config {
    int     enabled;                  /* 0=关闭 1=启用 */
    char    slot_dir[256];            /* 槽位根目录 */
    int     boot_attempt_max;         /* 最大启动尝试次数 */
    int     boot_confirm_sec;         /* 稳定运行确认时间（秒） */
    char    public_key[256];          /* 固件签名公钥路径（PEM，RSA/EC/Ed25519） */
    char    ca_file[256];             /* HTTPS 下载 CA bundle（PEM），空=系统默认 */
    char    ca_path[256];             /* HTTPS 下载 CA 目录（c_rehash 格式），可选 */
};

/* ─── HTTP Dashboard 配置 ──────────────────────────────── */

/* 与 http_config.reboot_token 同宽：常量时间比较的定长缓冲上限 */
#define HTTP_REBOOT_TOKEN_MAX   64

struct http_config {
    int     enabled;              /* 0=关闭 1=启用（默认 1，端口固定 8080） */
    char    reboot_token[HTTP_REBOOT_TOKEN_MAX];
                                /* POST /api/reboot 的认证 token。
                                 * 空串 = 未配置 → fail-closed，一律 403
                                 * （早期硬编码默认口令等于全网可重启设备） */
};

/* 配置结构 */
struct node_config {
    char    broker_host[128];
    int     broker_port;
    char    topic[128];
    char    client_id[64];
    int     debug_level;

    struct tls_config tls;        /* TLS + 安全 */
    struct modbus_config modbus;  /* Modbus 工业协议 */
    struct ota_config ota;        /* OTA 远程升级 */
    struct http_config http;      /* HTTP Dashboard */

    /*
     * 华为云 IoTDA 平台接入。
     * platform=local（缺省）时以下字段全部忽略，行为与历史 local 路径一致；
     * 缓冲尺寸按官方 device_id String(256)。
     */
    char    platform[16];            /* "local"（缺省）/ "huawei" */
    char    huawei_device_id[260];   /* 网关设备 device_id */
    char    huawei_secret[128];      /* 设备密钥（永不打日志，config_dump 打码） */
    int     huawei_auth_type;        /* 0=不校验时间戳（缺省）/ 1=校验 */
    int     huawei_keepalive;        /* MQTT keepalive 秒（钳制 30..1200，缺省 120） */
    int     huawei_props_interval;   /* 网关属性周期上报间隔秒（缺省 60） */
    int     subdev_offline_sec;      /* 子设备无数据判离线秒数（缺省 30） */
    char    subdevices_conf[256];    /* 子设备注册表路径（缺省 config/subdevices.conf） */
    char    huawei_ca_file[256];     /* 华为预置 CA 证书路径（TLS 锚点） */
};

/* 简单日志宏 */
#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN] " fmt "\n", ##__VA_ARGS__)

#endif /* COMMON_H */
