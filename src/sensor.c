/*
 * sensor.c
 * 传感器抽象层实现
 * 支持 mock（模拟数据）/ BMP280 / SHT30 三种模式
 * TODO: 真实 I2C 硬件驱动（BMP280/SHT30）
 */
#include "sensor.h"
#include <math.h>

static char g_sensor_type[32] = {0};

int sensor_init(const char *type) {
    if (!type || strlen(type) == 0) {
        LOG_ERROR("sensor type is empty");
        return E_INVAL;
    }

    strncpy(g_sensor_type, type, sizeof(g_sensor_type) - 1);

    if (strcmp(g_sensor_type, SENSOR_TYPE_MOCK) == 0) {
        LOG_INFO("sensor init: mock mode");
        return E_OK;
    }

    /* TODO: 真实硬件初始化 */
    if (strcmp(g_sensor_type, SENSOR_TYPE_BMP280) == 0) {
        LOG_INFO("sensor init: bmp280");
        return E_OK;
    }

    if (strcmp(g_sensor_type, SENSOR_TYPE_SHT30) == 0) {
        LOG_INFO("sensor init: sht30");
        return E_OK;
    }

    if (strcmp(g_sensor_type, SENSOR_TYPE_EX) == 0) {
        LOG_INFO("sensor init: extreme");
        return E_OK;
    }

    LOG_ERROR("unknown sensor type: %s", type);
    return E_INVAL;
}

int sensor_read(struct sensor_data *data) {
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
            data->temperature = 30.0 + (rand() % 1500) / 100.0;
            data->humidity = 50.0 + (rand() % 1500) / 100.0;
            data->pressure = 1010.0 + (rand() % 800) / 100.0;
        }
        return E_OK;
    } else if (strcmp(g_sensor_type, SENSOR_TYPE_EX) == 0) {
        /*极端环境 -10~80°C*/
        data->temperature = -10.0 + (rand() % 9000) / 100.0;
        data->humidity = 0.0 + (rand() % 10000) / 100.0;
        data->pressure = 1010.0 + (rand() % 800) / 100.0;
        return E_OK;
    }

    /* TODO: 真实 I2C 读取 */
    LOG_ERROR("real sensor read not implemented");
    return E_NOT_FOUND;
}

void sensor_close(void) { LOG_INFO("sensor closed"); }
