/*
 * tests/test_sensor.c
 * 传感器模块单元测试
 */
#include <stdio.h>
#include <assert.h>
#include "../src/sensor.h"

int main(void)
{
    struct sensor_data data;

    assert(sensor_init(SENSOR_TYPE_MOCK) == E_OK);

    for (int i = 0; i < 5; i++) {
        int rc = sensor_read(&data);
        assert(rc == E_OK);
        assert(data.temperature >= 20.0 && data.temperature <= 30.0);
        assert(data.humidity >= 40.0 && data.humidity <= 70.0);
        assert(data.pressure >= 1000.0 && data.pressure <= 1020.0);
        assert(data.timestamp_ms > 0);
        printf("test %d: temp=%.2f hum=%.2f pres=%.2f ts=%lld\n",
               i, data.temperature, data.humidity, data.pressure,
               (long long)data.timestamp_ms);
    }

    sensor_close();
    printf("sensor test passed\n");
    return 0;
}
