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

/* v1.3.0：华为云 IoTDA 接入（新增平台适配层 platform = local | huawei）。
 * 交付链：T01 数据层（哨兵统一 / source_id / 子设备注册表）→ T06 字段描述表单一事实源
 *        → T02 平台抽象 + local 等价回归 → T03 华为鉴权/属性/事件纯函数
 *        → T04 子设备管理 → T05 命令闭环与集成联调。
 * 不变式：platform=local 路径行为与 v1.2.11 **逐字节等价**（各测试即回归基线）。
 * 历史：v1.2.11 = P1-2 规则/异常引擎共享状态数据竞争修复（两引擎各加静态互斥锁，
 *       evaluate/get_stats 全程持锁，锁内不做 I/O；见 rule_engine.c / anomaly_engine.c）。 */
#define EMBMQTTNODE_VERSION "1.3.0"

/* 返回码 */
#define E_OK            0 // 成功  unix惯例 0为成功，非0为失败
#define E_INVAL        -1 //参数错误
#define E_NO_MEM       -2 //内存不足
#define E_IO           -3 //IO错误
#define E_NET          -4 //网络错误
#define E_TIMEOUT      -5 //超时
#define E_NOT_FOUND    -6 // 未找到

/* 传感器数据来源（集中定义：local 板载传感器 / modbus 从站） */
typedef enum sensor_source {
    SOURCE_LOCAL  = 0, /* 本地板载传感器 */
    SOURCE_MODBUS = 1, /* Modbus 从站数据 */
} sensor_source_t;

/* 全项目唯一的"无此字段 / 无效值"哨兵（docs/12 §3.5）。
 *
 * 语义：任何字段值 == SENSOR_VALUE_INVALID 一律按"无效值"处理——
 *   - 引擎（rule/anomaly）判定时该字段永不匹配（不参与规则/异常判定）；
 *   - huawei builder 侧整体省略该字段不上报（方案 A mask，local 路径不 mask）。
 *
 * 历史 bug（本宏统一后消除）：SHT30 无气压测量曾写 -1.0（sensor.c），
 * 而引擎判定只拦 -999.0 → pressure=-1.0 被当真实气压参与规则/异常判定，
 * 并被当作有效值上报云端。统一引用本宏后，哨兵赋值点与判定点一致。 */
#define SENSOR_VALUE_INVALID (-999.0)

/* 传感器数据结构 */
struct sensor_data {
    double    temperature;   /* 摄氏度 */
    double    humidity;      /* %RH，无有效读数时 = SENSOR_VALUE_INVALID */
    double    pressure;      /* hPa，无有效读数时 = SENSOR_VALUE_INVALID */
    int64_t   timestamp_ms;  /* 毫秒时间戳（采样时刻，事件 event_time 源） */
    int64_t   id;            /* SQLite 自增主键，0 表示未持久化（storage_save 成功后回填） */
    sensor_source_t source;  /* 数据来源（storage_get_pending 读出，供补发时选择 topic） */
    int       source_id;     /* 数据源实例标识（供 huawei 子设备注册表路由）：
                              *   SOURCE_LOCAL  恒 0（本地板载传感器）；
                              *   SOURCE_MODBUS 取从站地址 slave_id 1..247；
                              *   未来 BLE 等数据源另占位（docs/12 §5 数据源无关键）。 */
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

/* ─── 规则引擎配置（阶段三）───────────────────────────────── */

#define RULE_MAX            32
#define RULE_NAME_LEN       32
#define RULE_FIELD_LEN      32
#define RULE_ACTION_LEN     64

/* 规则运算符 */
enum rule_op {
    OP_GT,       /* >  大于 */
    OP_LT,       /* <  小于 */
    OP_EQ,       /* == 等于 */
    OP_NE,       /* != 不等于 */
    OP_OUT,  /* 区间外 (值 < lo 或 值 > hi) */
    OP_RATE,     /* 瞬时变化率超过阈值（单位/秒） */
};

/* 告警动作位掩码 */
#define ACTION_LOG_ONLY   0x01
#define ACTION_ALERT_MQTT 0x02
#define ACTION_GPIO_1     0x04
#define ACTION_GPIO_2     0x08

/* 单条规则定义 */
struct rule {
    char        name[RULE_NAME_LEN];
    char        field[RULE_FIELD_LEN];    /* temperature/humidity/pressure */
    enum rule_op op;
    double      threshold;                /* gt/lt/eq/ne/rate 的阈值 */
    double      threshold_lo;             /* outside 下界 */
    double      threshold_hi;             /* outside 上界 */
    uint8_t     action_mask;              /* 触发时执行的动作 */
    int         cooldown_ms;              /* 冷却时间（防重复告警） */
    /* ── 运行时状态（内部使用）── */
    int64_t     last_triggered;           /* 上次触发时间戳 (ms) */
    /* rate 操作环形缓冲区 */
    double      rate_history[16];
    int64_t     rate_timestamps[16];
    int         rate_head;
    int         rate_count;
};

/* 规则引擎统计（供 dashboard 查询） */
struct rule_stats {
    char        name[RULE_NAME_LEN]; /*规则名称*/
    int         trigger_count;      /*触发次数*/
    int64_t     last_triggered;     /*上次触发时间戳(ms)*/
};

/* ─── GPIO 常量 ───────────────────────────────────────────── */
#define GPIO_PIN_MAX 4

/* ─── 异常检测引擎配置（方向 B）─────────────────────────── */

#define ANOMALY_MAX            16
#define ANOMALY_WINDOW_SIZE    128
#define ANOMALY_NAME_LEN       32
#define ANOMALY_FIELD_LEN      32

/* 异常检测算法 */
enum anomaly_algo {
    ANOMALY_ZSCORE  = 0,   /* Z-score 统计方法 */
    ANOMALY_IFOREST = 1,   /* Isolation Forest 机器学习 */
};

/* 单条异常检测规则定义 */
struct anomaly_config {
    char    name[ANOMALY_NAME_LEN];
    char    field[ANOMALY_FIELD_LEN];     /* temperature/humidity/pressure */
    enum anomaly_algo algo;
    double  zscore_threshold;             /* Z-score 阈值（默认 3.0） */
    int     window_size;                  /* 滑动窗口大小（最大 128） */
    uint8_t action_mask;                  /* 触发时执行的动作 */
    int     cooldown_ms;                  /* 冷却时间 */
    int     iforest_enabled;              /* Isolation Forest 是否启用 */
    /* ── 运行时状态（内部使用）── */
    double  window[ANOMALY_WINDOW_SIZE];
    int     window_head;
    int     window_count;
    double  baseline_mean;
    double  baseline_std;
    int64_t last_triggered;
    int     trigger_count;
};

/* 异常检测统计（供 dashboard 查询） */
struct anomaly_stats {
    char    name[ANOMALY_NAME_LEN];
    int     trigger_count;
    int64_t last_triggered;
    double  current_zscore;               /* 最近一次计算的 z-score */
    double  current_score;                /* iForest 异常分数 (0-1) */
};

/* ─── 结构化告警事件（P1-1；docs/12 §3.2）──────────────────
 *
 * rule/anomaly 引擎在触发时，除沿用的 msg 文本外，额外填充本结构，
 * 供平台层（huawei 物模型事件）消费。local 路径只用 msg 字段，
 * 行为与 v1.2.11 完全一致（msg 文本逐字节不变）。 */
struct alert_event {
    char    rule_name[32];   /* 触发规则/异常名 */
    char    field[32];       /* 触发字段名 */
    double  value;           /* 触发时的字段值 */
    double  threshold;       /* 触发阈值（outside 取区间下界） */
    char    source_kind[16]; /* "sensor"（source_id==0）/ "modbus"（source_id!=0） */
    int     source_id;       /* 数据源实例：modbus slave_id / 本地传感器 0 */
    char    msg[256];        /* 引擎生成文本，与 v1.2.11 相同 */
    int64_t ts_ms;           /* 采样时刻（事件 event_time 源） */
};

/* ─── 严格 JSON 字符串字段提取（P1-7/P1-8/P1-6 共用）────────── */

/*
 * json_get_string - 从 JSON 文本中严格提取字符串字段的值。
 *
 * 参数:
 *   json/json_len  输入缓冲区及其长度（不要求 NUL 结尾）
 *   key            字段名（不含两侧引号，如 "cmd"）
 *   out/outsz      输出缓冲（成功时保证 NUL 结尾）
 *
 * 返回: 1 = 找到并完整提取；0 = 未找到 / 格式非法 / 值过长（拒绝而非截断）
 *
 * 严格性（针对 P1-7/P1-8 的修复语义）:
 *   - 只在 [0, json_len) 内扫描，绝不依赖 NUL 结尾：MQTT payload
 *     是带长度的字节序列而非 C 字符串，旧 strstr 写法会越界读
 *   - key 必须处于"键位置"（左侧最近的非空白字符为 '{' 或 ','），
 *     值字符串里出现的 "key" 字样不会误匹配
 *   - 值必须是带引号的 JSON 字符串；解析 \" \\ \/ \b \f \n \r \t
 *     转义；遇到不认识的转义（含 \uXXXX，内部指令不需要）返回 0，
 *     宁可拒绝也不猜测
 *   - 值内出现裸控制字符（< 0x20，JSON 规范禁止）返回 0
 *   - 解码后长度超过 outsz-1 返回 0：绝不静默截断（截断的 URL /
 *     checksum 会把攻击面悄悄放大）
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

/* ─── OTA 远程升级配置（阶段四）────────────────────────────── */

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

/* OTA 配置（从配置文件读取） */
struct ota_config {
    int     enabled;                  /* 0=关闭 1=启用 */
    char    slot_dir[256];            /* 槽位根目录 */
    int     boot_attempt_max;         /* 最大启动尝试次数 */
    int     boot_confirm_sec;         /* 稳定运行确认时间（秒），超时后确认本次启动为健康 */
    /* v1.2.9 固件签名 + HTTPS 下载（fail-closed 语义详见 src/ota.c 头注释）：
     * public_key 空串=未配置 → OTA 升级指令直接拒绝（不降级到 checksum-only）；
     * 已配置（含生产默认值）→ 验签为硬性关卡，.sig 缺失/签名不匹配/公钥
     * 不可读一律拒绝安装。ca_file/ca_path 控制 https 下载的证书校验锚点，
     * 两者均空时尝试系统 CA 常见位置，找不到即拒绝 https 下载。 */
    char    public_key[256];          /* 固件签名公钥路径（PEM，RSA/EC/Ed25519） */
    char    ca_file[256];             /* HTTPS 下载 CA 证书 bundle（PEM），空=系统默认 */
    char    ca_path[256];             /* HTTPS 下载 CA 目录（c_rehash 格式），可选 */
};

/* ─── HTTP Dashboard 配置（P1-6）────────────────────────── */

/* HTTP_REBOOT_TOKEN_MAX 与 http_config.reboot_token 同宽：
 * 常量时间比较的定长缓冲上限 */
#define HTTP_REBOOT_TOKEN_MAX   64

struct http_config {
    int     enabled;              /* 0=关闭 1=启用（默认 1，端口固定 8080） */
    char    reboot_token[HTTP_REBOOT_TOKEN_MAX];
                                /* POST /api/reboot 的认证 token。
                                 * 空串 = 未配置 → fail-closed，
                                 * 一律 403 拒绝（v1.2.9 前硬编码
                                 * "reboot123" 等于全网可重启设备） */
};

/* 配置结构 */
struct node_config {
    char    broker_host[128];
    int     broker_port;
    char    topic[128];
    char    client_id[64];
    int     sample_interval_ms;
    char    sensor_type[32];
    char    sensor_i2c_dev[64];     /* I2C 适配器路径，如 /dev/i2c-1 */
    int     debug_level;

    /* TLS + 安全 */
    struct tls_config tls;

    /* Modbus 工业协议 */
    struct modbus_config modbus;

    /* 规则引擎 */
    int         rule_count;
    struct rule rules[RULE_MAX];

    /* OTA 远程升级 */
    struct ota_config ota;

    /* HTTP Dashboard（P1-6）*/
    struct http_config http;

    /* 异常检测引擎 */
    int         anomaly_enabled;
    int         anomaly_count;
    struct      anomaly_config anoms[ANOMALY_MAX];

    /* ─── 华为云 IoTDA 平台接入（v1.3.0 适配层，T01）──────────
     * platform=local（缺省）时以下字段全部忽略，行为与 v1.2.11 完全一致。
     * 缓冲尺寸依据 docs/12 §3.1/§5.2（官方 device_id String(256)）。 */
    char    platform[16];            /* "local"（缺省）/ "huawei" */
    char    huawei_device_id[260];   /* 网关设备 device_id（官方 String(256)） */
    char    huawei_secret[128];      /* 设备密钥（永不打日志，config_dump 打码） */
    int     huawei_auth_type;        /* 签名类型：0=不校验时间戳（缺省）/ 1=校验 */
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
