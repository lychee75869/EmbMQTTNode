/*
 * sensor.c
 * 传感器抽象层实现
 * 支持 mock（模拟数据） / ads1115 / sht30
 *
 * 真实硬件通过 Linux 用户态 I2C 接口（/dev/i2c-N）访问：
 *   - ads1115 : 板载 ADC（地址 0x48），4 路单端模拟输入
 *               AIN0/AIN1/AIN2 → temperature/humidity/pressure 三个字段，
 *               数值为电压（V），配合外部电压输出型传感器 + scale 使用
 *   - sht30   : 温湿度传感器（地址 0x44），单次测量命令 0x2C06
 */
#include "sensor.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

/* ─── I2C 设备地址 ─────────────────────────────────────── */
#define ADS1115_ADDR_DEFAULT   0x48
#define SHT30_ADDR_DEFAULT     0x44

/* ─── ADS1115 寄存器 ───────────────────────────────────── */
#define ADS1115_REG_CONVERSION 0x00
#define ADS1115_REG_CONFIG     0x01

/* ADS1115 配置字：单次转换 | 单端 AIN(n) | PGA ±4.096V | 单次模式 | 128SPS | 比较器关闭 */
#define ADS1115_CFG_BASE       0x8000  /* OS=1 启动转换          */
#define ADS1115_CFG_PGA_4096   0x0200  /* PGA ±4.096V            */
#define ADS1115_CFG_SINGLE     0x0100  /* 单次转换模式           */
#define ADS1115_CFG_COMP_OFF   0x0003  /* 比较器禁用             */
#define ADS1115_MUX_AIN(n)     (0x4000 | ((uint16_t)(n) << 12))
#define ADS1115_PGA_FS         4.096   /* 满量程电压 (V)         */

/* ─── 内部状态 ─────────────────────────────────────────── */
static char g_sensor_type[32] = {0};
static char g_i2c_dev[64]     = {0};
static int  g_i2c_fd          = -1;

/* ════════════════════════════════════════════════════════════
 * I2C 底层读写助手
 * ════════════════════════════════════════════════════════════ */

/* 打开 I2C 适配器并选定从机地址 */
static int i2c_open(const char *dev, uint8_t addr)
{
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        LOG_ERROR("open %s failed: %s", dev, strerror(errno));
        return -1;
    }
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        LOG_ERROR("ioctl I2C_SLAVE(0x%02X) on %s failed: %s",
                  addr, dev, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* 向寄存器 reg 写 16 位值（大端） */
static int i2c_write_reg16(int fd, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
    if (write(fd, buf, 3) != 3) {
        LOG_ERROR("i2c write reg 0x%02X failed: %s", reg, strerror(errno));
        return -1;
    }
    return 0;
}

/* 从寄存器 reg 读 len 字节 */
static int i2c_read_regs(int fd, uint8_t reg, uint8_t *buf, int len)
{
    if (write(fd, &reg, 1) != 1) {
        LOG_ERROR("i2c set pointer 0x%02X failed: %s", reg, strerror(errno));
        return -1;
    }
    if (read(fd, buf, len) != len) {
        LOG_ERROR("i2c read reg 0x%02X failed: %s", reg, strerror(errno));
        return -1;
    }
    return 0;
}

/* ════════════════════════════════════════════════════════════
 * ADS1115 驱动（板载，地址 0x48）
 * ════════════════════════════════════════════════════════════ */

/* 启动一次单端转换并返回原始值（有符号 16 位） */
static int ads1115_read_raw(int fd, int channel, int16_t *raw)
{
    if (channel < 0 || channel > 3)
        return -1;

    uint16_t cfg = ADS1115_CFG_BASE | ADS1115_MUX_AIN(channel) |
                   ADS1115_CFG_PGA_4096 | ADS1115_CFG_SINGLE |
                   ADS1115_CFG_COMP_OFF;

    /* 写配置寄存器，OS 位置 1 立即启动转换 */
    if (i2c_write_reg16(fd, ADS1115_REG_CONFIG, cfg) != 0)
        return -1;

    /* 128 SPS 单次模式，一次转换约 8ms，等 10ms 足够 */
    usleep(10000);

    uint8_t buf[2];
    if (i2c_read_regs(fd, ADS1115_REG_CONVERSION, buf, 2) != 0)
        return -1;

    *raw = (int16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    return 0;
}

/* 读取一路电压（V），PGA=±4.096V：1 LSB = 4.096/32768 V */
static int ads1115_read_volt(int fd, int channel, double *volt)
{
    int16_t raw;
    if (ads1115_read_raw(fd, channel, &raw) != 0)
        return -1;
    *volt = (double)raw * ADS1115_PGA_FS / 32768.0;
    return 0;
}

/*
 * ADS1115 采集：4 路单端通道映射到 sensor_data
 *   AIN0 → temperature 字段
 *   AIN1 → humidity    字段
 *   AIN2 → pressure    字段
 *   AIN3 → 暂不采集（后续可作为第四路扩展）
 * 数值含义为该通道输入电压（V）——语义由外接传感器决定。
 */
static int ads1115_sensor_read(struct sensor_data *data)
{
    double v0, v1, v2;

    if (ads1115_read_volt(g_i2c_fd, 0, &v0) != 0) return E_IO;
    if (ads1115_read_volt(g_i2c_fd, 1, &v1) != 0) return E_IO;
    if (ads1115_read_volt(g_i2c_fd, 2, &v2) != 0) return E_IO;

    data->temperature = v0;
    data->humidity    = v1;
    data->pressure    = v2;
    return E_OK;
}

/* ════════════════════════════════════════════════════════════
 * SHT30 驱动（地址 0x44）
 * ════════════════════════════════════════════════════════════ */

/* SHT30 CRC-8：多项式 0x31，初值 0xFF */
static uint8_t sht30_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31)
                               : (uint8_t)(crc << 1);
    }
    return crc;
}

static int sht30_sensor_read(struct sensor_data *data)
{
    uint8_t cmd[2] = { 0x2C, 0x06 }; /* 单次测量：高重复性、无时钟拉伸 */
    uint8_t buf[6];

    /* 发送测量命令 */
    if (write(g_i2c_fd, cmd, 2) != 2) {
        LOG_ERROR("sht30 write cmd failed: %s", strerror(errno));
        return E_IO;
    }

    /* 高重复性测量耗时最长约 15ms，等待转换完成 */
    usleep(20000);

    /* 读回：温度 2 字节 + CRC + 湿度 2 字节 + CRC */
    if (read(g_i2c_fd, buf, 6) != 6) {
        LOG_ERROR("sht30 read data failed: %s", strerror(errno));
        return E_IO;
    }

    if (sht30_crc8(&buf[0], 2) != buf[2] ||
        sht30_crc8(&buf[3], 2) != buf[5]) {
        LOG_ERROR("sht30 crc check failed");
        return E_IO;
    }

    uint16_t raw_t = ((uint16_t)buf[0] << 8) | buf[1];
    uint16_t raw_h = ((uint16_t)buf[3] << 8) | buf[4];

    /* 数据手册换算公式（高重复性） */
    data->temperature = -45.0 + 175.0 * (double)raw_t / 65535.0;
    data->humidity    = 100.0 * (double)raw_h / 65535.0;
    data->pressure    = -1.0; /* SHT30 无气压测量 */
    return E_OK;
}

/* ════════════════════════════════════════════════════════════
 * 传感器抽象层公共接口
 * ════════════════════════════════════════════════════════════ */

int sensor_init(const char *type, const char *i2c_dev)
{
    if (!type || strlen(type) == 0) {
        LOG_ERROR("sensor type is empty");
        return E_INVAL;
    }

    strncpy(g_sensor_type, type, sizeof(g_sensor_type) - 1);
    if (i2c_dev)
        strncpy(g_i2c_dev, i2c_dev, sizeof(g_i2c_dev) - 1);

    if (strcmp(g_sensor_type, SENSOR_TYPE_MOCK) == 0) {
        LOG_INFO("sensor init: mock mode");
        return E_OK;
    }

    /* ── 以下为真实硬件模式，需要 I2C 设备路径 ── */
    if (strlen(g_i2c_dev) == 0) {
        LOG_ERROR("sensor_type=%s requires sensor_i2c_dev (e.g. /dev/i2c-1)",
                  g_sensor_type);
        return E_INVAL;
    }

    if (strcmp(g_sensor_type, SENSOR_TYPE_ADS1115) == 0) {
        g_i2c_fd = i2c_open(g_i2c_dev, ADS1115_ADDR_DEFAULT);
        if (g_i2c_fd < 0)
            return E_IO;
        LOG_INFO("sensor init: ads1115 on %s addr 0x%02X (AIN0/1/2 -> "
                 "temperature/humidity/pressure, unit: Volt)",
                 g_i2c_dev, ADS1115_ADDR_DEFAULT);
        return E_OK;
    }

    if (strcmp(g_sensor_type, SENSOR_TYPE_SHT30) == 0) {
        g_i2c_fd = i2c_open(g_i2c_dev, SHT30_ADDR_DEFAULT);
        if (g_i2c_fd < 0)
            return E_IO;
        LOG_INFO("sensor init: sht30 on %s addr 0x%02X",
                 g_i2c_dev, SHT30_ADDR_DEFAULT);
        return E_OK;
    }

    LOG_ERROR("unknown sensor type: %s", type);
    return E_INVAL;
}

int sensor_read(struct sensor_data *data)
{
    if (!data)
        return E_INVAL;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    data->timestamp_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    if (strcmp(g_sensor_type, SENSOR_TYPE_MOCK) == 0) {
        /* 模拟数据：正常基线 + 每第 4 次采样注入异常 */
        static int g_inject_counter = 0;
        g_inject_counter++;

        if (g_inject_counter % 4 == 0) {
            /* 异常注入：模拟传感器故障 / 环境失控 */
            data->temperature = 42.0 + (rand() % 600) / 100.0; /* 42-48°C */
            data->humidity = 8.0 + (rand() % 700) / 100.0;     /* 8-15%  */
            data->pressure = 975.0 + (rand() % 800) / 100.0;   /* 975-983 hPa */
        } else {
            /* 正常基线：温度 23-27℃，湿度 50-65%，气压 1010-1018 hPa */
            data->temperature = 23.0 + (rand() % 400) / 100.0;
            data->humidity = 50.0 + (rand() % 1500) / 100.0;
            data->pressure = 1010.0 + (rand() % 800) / 100.0;
        }
        return E_OK;
    } else if (strcmp(g_sensor_type, SENSOR_TYPE_ADS1115) == 0) {
        return ads1115_sensor_read(data);
    } else if (strcmp(g_sensor_type, SENSOR_TYPE_SHT30) == 0) {
        return sht30_sensor_read(data);
    }

    LOG_ERROR("real sensor read not implemented for type: %s",
              g_sensor_type);
    return E_NOT_FOUND;
}

void sensor_close(void)
{
    if (g_i2c_fd >= 0) {
        close(g_i2c_fd);
        g_i2c_fd = -1;
    }
    LOG_INFO("sensor closed");
}
