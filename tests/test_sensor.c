/*
 * tests/test_sensor.c
 * 传感器模块单元测试
 *
 * 覆盖:
 *   - mock 模式初始化
 *   - 传感器数据读取（温度/湿度/气压范围校验）
 *   - 时间戳有效性
 *   - 传感器关闭
 */
#include <stdio.h>
#include <assert.h>
#include "../src/sensor.h"

/* ═══════════════════════════════════════════════════════════ */

int main(void)
{
    struct sensor_data data;

    assert(sensor_init(SENSOR_TYPE_MOCK) == E_OK);

    /*
     * mock 传感器每 4 次采样注入一次异常（模拟传感器故障）。
     * 此处测试 8 次读取：验证正常样本在基线范围内，
     * 异常样本（每第 4 次）超出正常范围。
     */
    int normal_ok = 0, anomaly_detected = 0;
    for (int i = 0; i < 8; i++) {
        int rc = sensor_read(&data);
        assert(rc == E_OK);
        assert(data.timestamp_ms > 0);

        if ((i + 1) % 4 == 0) {
            /* 异常注入样本：范围外 */
            assert(data.temperature > 30.0 || data.humidity < 40.0 ||
                   data.pressure < 1000.0);
            anomaly_detected++;
        } else {
            /* 正常样本：基线范围内 */
            assert(data.temperature >= 23.0 && data.temperature <= 27.0);
            assert(data.humidity >= 50.0 && data.humidity <= 65.0);
            assert(data.pressure >= 1010.0 && data.pressure <= 1018.0);
            normal_ok++;
        }
        printf("test %d: temp=%.2f hum=%.2f pres=%.2f ts=%lld [%s]\n",
               i, data.temperature, data.humidity, data.pressure,
               (long long)data.timestamp_ms,
               ((i + 1) % 4 == 0) ? "ANOMALY" : "normal");
    }

    assert(normal_ok == 6);
    assert(anomaly_detected == 2);
    sensor_close();
    printf("sensor test passed (normal=%d, anomaly=%d)\n",
           normal_ok, anomaly_detected);
    return 0;
}
