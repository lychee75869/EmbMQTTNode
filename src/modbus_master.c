/*
 * modbus_master.c
 * Modbus 主站协议模块（RTU + TCP）
 *
 * 阶段二：工业协议接入
 * - 基于 libmodbus 实现 RTU / TCP 两种模式
 * - 支持 int16 / uint16 / float32 / int32 四种寄存器数据类型
 * - 当 BUILD_WITH_MODBUS 未定义或 modbus 禁用时，自动进入 mock 模式
 */

#include "modbus_master.h"

#ifdef BUILD_WITH_MODBUS
#include <modbus/modbus.h>
#endif

/* ─── 内部状态 ─────────────────────────────────────────── */

static int g_modbus_connected = 0;            /* 连接状态标志 */
static struct modbus_config g_modbus_cfg;     /* 本地配置副本 */

#ifdef BUILD_WITH_MODBUS
static modbus_t *g_mb_ctx = NULL;             /* libmodbus 上下文 */
#endif

/* ─── 字段映射辅助 ─────────────────────────────────────── */

/*
 * 将寄存器值写入 sensor_data 的指定字段
 * raw: 原始值（已做 scale+offset 转换）
 */
static void set_field(struct sensor_data *data,
                      const char *field_name, double raw)
{
    if (strcmp(field_name, "temperature") == 0)
        data->temperature = raw;
    else if (strcmp(field_name, "humidity") == 0)
        data->humidity = raw;
    else if (strcmp(field_name, "pressure") == 0)
        data->pressure = raw;
    else
        LOG_WARN("modbus: unknown field '%s'", field_name);
}

#ifdef BUILD_WITH_MODBUS

/*
 * 将两个 uint16 寄存器拼接为 float32（IEEE 754）
 */
static double uint16_to_float32(uint16_t hi, uint16_t lo)
{
    union {
        uint32_t u32;
        float    f32;
    } conv;

    /* 字节序：大端（Modbus 标准），hi 在前 */
    conv.u32 = ((uint32_t)hi << 16) | (uint32_t)lo;
    return (double)conv.f32;
}

/*
 * 将两个 uint16 寄存器拼接为 int32
 */
static double uint16_to_int32(uint16_t hi, uint16_t lo)
{
    union {
        uint32_t u32;
        int32_t  i32;
    } conv;

    conv.u32 = ((uint32_t)hi << 16) | (uint32_t)lo;
    return (double)conv.i32;
}

/*
 * 将 uint16 转为 int16（处理符号位）
 */
static double uint16_to_int16(uint16_t val)
{
    union {
        uint16_t u16;
        int16_t  i16;
    } conv;
    conv.u16 = val;
    return (double)conv.i16;
}

/*
 * 将原始寄存器值按 data_type 转为物理量
 */
static double convert_register(const uint16_t *regs, int reg_count,
                               const char *data_type)
{
    if (strcmp(data_type, "uint16") == 0) {
        return (double)regs[0];
    } else if (strcmp(data_type, "int16") == 0) {
        return uint16_to_int16(regs[0]);
    } else if (strcmp(data_type, "float32") == 0) {
        if (reg_count < 2) {
            LOG_ERROR("modbus: float32 needs 2 registers, got %d", reg_count);
            return 0.0;
        }
        return uint16_to_float32(regs[0], regs[1]);
    } else if (strcmp(data_type, "int32") == 0) {
        if (reg_count < 2) {
            LOG_ERROR("modbus: int32 needs 2 registers, got %d", reg_count);
            return 0.0;
        }
        return uint16_to_int32(regs[0], regs[1]);
    } else {
        LOG_WARN("modbus: unknown data_type '%s', using uint16", data_type);
        return (double)regs[0];
    }
}

#endif /* BUILD_WITH_MODBUS */

/* ─── Mock 模式 ─────────────────────────────────────────── */

/*
 * 模拟模式：生成随机传感器数据（开发阶段无需硬件）
 */
static int mock_poll(struct sensor_data *data, int max_count)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    int n = 0;
    for (int i = 0; i < g_modbus_cfg.reg_count && n < max_count; i++) {
        struct modbus_reg_map *reg = &g_modbus_cfg.regs[i];

        data[n].timestamp_ms = (int64_t)ts.tv_sec * 1000
                               + ts.tv_nsec / 1000000;
        data[n].temperature = -999.0;   /* 未定义标记 */
        data[n].humidity    = -999.0;
        data[n].pressure    = -999.0;

        /* 根据 field_name 填入模拟值 */
        double raw;
        if (strcmp(reg->field_name, "temperature") == 0)
            raw = 20.0 + (rand() % 1000) / 100.0;
        else if (strcmp(reg->field_name, "humidity") == 0)
            raw = 40.0 + (rand() % 3000) / 100.0;
        else if (strcmp(reg->field_name, "pressure") == 0)
            raw = 1000.0 + (rand() % 2000) / 100.0;
        else
            raw = (rand() % 10000) / 100.0;  /* 通用随机值 */

        set_field(&data[n], reg->field_name, raw);
        n++;
    }
    return n;
}

/* ─── 公开 API ──────────────────────────────────────────── */

int modbus_master_init(const struct modbus_config *cfg)
{
    if (!cfg) return E_INVAL;

    /* 保存配置副本 */
    memcpy(&g_modbus_cfg, cfg, sizeof(*cfg));

    if (!cfg->enabled) {
        LOG_INFO("modbus: disabled by config, using mock mode");
        g_modbus_connected = 0;
        return E_OK;
    }

#ifdef BUILD_WITH_MODBUS
    /* ── 根据模式创建上下文 ── */
    if (strcmp(cfg->mode, "tcp") == 0) {
        g_mb_ctx = modbus_new_tcp(cfg->tcp_host, cfg->tcp_port);
        LOG_INFO("modbus tcp: %s:%d", cfg->tcp_host, cfg->tcp_port);
    } else if (strcmp(cfg->mode, "rtu") == 0) {
        g_mb_ctx = modbus_new_rtu(cfg->serial_port,
                                  cfg->baudrate,
                                  cfg->parity[0],
                                  cfg->data_bits,
                                  cfg->stop_bits);
        LOG_INFO("modbus rtu: %s %d %c%d%c",
                 cfg->serial_port, cfg->baudrate,
                 cfg->parity[0], cfg->data_bits, cfg->stop_bits);
    } else {
        LOG_ERROR("modbus: unknown mode '%s'", cfg->mode);
        g_modbus_connected = 0;
        return E_INVAL;
    }

    if (!g_mb_ctx) {
        LOG_ERROR("modbus: failed to create context (libmodbus not available?)");
        g_modbus_connected = 0;
        return E_NET;
    }

    /* ── 连接 ── */
    if (modbus_connect(g_mb_ctx) < 0) {
        LOG_ERROR("modbus_connect failed: %s",
                  modbus_strerror(errno));
        modbus_free(g_mb_ctx);
        g_mb_ctx = NULL;
        g_modbus_connected = 0;
        LOG_WARN("modbus: falling back to mock mode");
        return E_NET;
    }

    g_modbus_connected = 1;
    LOG_INFO("modbus master init ok, %d register mappings",
             cfg->reg_count);
    return E_OK;

#else
    /* libmodbus 未编译进项目，自动 mock */
    LOG_INFO("modbus: BUILD_WITH_MODBUS not set, using mock mode");
    g_modbus_connected = 0;
    return E_OK;
#endif
}

int modbus_master_poll(struct sensor_data *data, int max_count)
{
    if (!data || max_count <= 0) return E_INVAL;

    /* 未连接：走 mock */
    if (!g_modbus_connected) {
        return mock_poll(data, max_count);
    }

#ifdef BUILD_WITH_MODBUS
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    int n = 0;
    for (int i = 0; i < g_modbus_cfg.reg_count && n < max_count; i++) {
        struct modbus_reg_map *reg = &g_modbus_cfg.regs[i];

        /* 设置从站地址 */
        if (modbus_set_slave(g_mb_ctx, reg->slave_id) < 0) {
            LOG_ERROR("modbus: set_slave %d failed: %s",
                      reg->slave_id, modbus_strerror(errno));
            continue;
        }

        /* 根据功能码读取 */
        uint16_t reg_buf[MODBUS_REG_MAX];
        int rc;
        if (reg->func_code == 3) {
            rc = modbus_read_registers(g_mb_ctx,
                                       reg->reg_addr - 40001,
                                       reg->reg_count, reg_buf);
        } else if (reg->func_code == 4) {
            rc = modbus_read_input_registers(g_mb_ctx,
                                             reg->reg_addr - 30001,
                                             reg->reg_count, reg_buf);
        } else {
            LOG_ERROR("modbus: unsupported func_code %d for slave %d",
                      reg->func_code, reg->slave_id);
            continue;
        }

        if (rc < 0) {
            LOG_ERROR("modbus: read slave=%d addr=%d count=%d failed: %s",
                      reg->slave_id, reg->reg_addr,
                      reg->reg_count, modbus_strerror(errno));
            continue;
        }

        /* 数据类型转换 */
        double raw = convert_register(reg_buf, reg->reg_count,
                                      reg->data_type);

        /* 物理量转换 */
        double physical = raw * reg->scale + reg->offset;

        /* 填充 sensor_data */
        memset(&data[n], 0, sizeof(data[n]));
        data[n].timestamp_ms = (int64_t)ts.tv_sec * 1000
                               + ts.tv_nsec / 1000000;
        data[n].temperature = -999.0;
        data[n].humidity    = -999.0;
        data[n].pressure    = -999.0;
        set_field(&data[n], reg->field_name, physical);

        n++;
    }
    return n;
#else
    return mock_poll(data, max_count);
#endif
}

int modbus_master_is_connected(void)
{
    return g_modbus_connected;
}

void modbus_master_close(void)
{
#ifdef BUILD_WITH_MODBUS
    if (g_mb_ctx) {
        modbus_close(g_mb_ctx);
        modbus_free(g_mb_ctx);
        g_mb_ctx = NULL;
    }
#endif
    g_modbus_connected = 0;
    LOG_INFO("modbus master closed");
}