#include "sensor.h"
#include <math.h>

static char g_sensor_type[32] = {0};

int sensor_init(const char *type)
{
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
        LOG_INFO("sensor init: bmp280 (real hardware mode, not implemented in simulation)");
        return E_OK;
    }

    if (strcmp(g_sensor_type, SENSOR_TYPE_SHT30) == 0) {
        LOG_INFO("sensor init: sht30 (real hardware mode, not implemented in simulation)");
        return E_OK;
    }

    LOG_ERROR("unknown sensor type: %s", type);
    return E_INVAL;
}

int sensor_read(struct sensor_data *data)
{
    if (!data) return E_INVAL;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    data->timestamp_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    if (strcmp(g_sensor_type, SENSOR_TYPE_MOCK) == 0) {
        /* 模拟数据：温度 20-30℃，湿度 40-70%，气压 1000-1020 hPa */
        data->temperature = 20.0 + (rand() % 1000) / 100.0;
        data->humidity    = 40.0 + (rand() % 3000) / 100.0;
        data->pressure    = 1000.0 + (rand() % 2000) / 100.0;
        return E_OK;
    }

    /* TODO: 真实 I2C 读取 */
    LOG_ERROR("real sensor read not implemented");
    return E_NOT_FOUND;
}

void sensor_close(void)
{
    LOG_INFO("sensor closed");
}
