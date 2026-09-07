/*
 * tests/test_storage.c
 * 存储模块单元测试
 *
 * 覆盖:
 *   - SQLite 数据库初始化
 *   - 传感器数据保存（含 source 来源、id 主键回填）
 *   - 待发送数据查询（ORDER BY timestamp ASC，返回 id + source）
 *   - 按主键精确删除单条（storage_delete_by_id，v1.2.4）
 *   - legacy 按时间戳删除（storage_delete_sent）
 *   - 混源数据保存与读回（local / modbus）
 *   - 数据库关闭
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../src/storage.h"

/* ═══════════════════════════════════════════════════════════ */

int main(void)
{
    struct sensor_data data[3];
    struct sensor_data out[3];

    /* 清理历史测试数据 */
    remove("test_data.db");
    assert(storage_init("test_data.db") == E_OK);

    /* ── 1. 保存 3 条 local 数据，验证 id 回填 ── */
    for (int i = 0; i < 3; i++) {
        memset(&data[i], 0, sizeof(data[i]));
        data[i].temperature = 20.0 + i;
        data[i].humidity = 50.0 + i;
        data[i].pressure = 1010.0 + i;
        data[i].timestamp_ms = 1000 + i;
        data[i].source = SOURCE_LOCAL;
        assert(storage_save(&data[i], SOURCE_LOCAL, "test-client") == E_OK);
        assert(data[i].id > 0); /* save 成功后回填自增主键 */
    }

    /* ── 2. get_pending 读回，验证字段 + id + source ── */
    int n = storage_get_pending(out, 3);
    assert(n == 3);
    assert(out[0].timestamp_ms == 1000);
    assert(out[2].timestamp_ms == 1002);
    for (int i = 0; i < 3; i++) {
        assert(out[i].id > 0);
        assert(out[i].source == SOURCE_LOCAL);
        assert(out[i].temperature == 20.0 + i);
        assert(out[i].humidity == 50.0 + i);
        assert(out[i].pressure == 1010.0 + i);
    }

    /* ── 3. 逐条按 id 删除 → 0 条 ── */
    for (int i = 0; i < 3; i++) {
        assert(storage_delete_by_id(out[i].id) == E_OK);
    }
    n = storage_get_pending(out, 3);
    assert(n == 0);

    /* ── 4. 混源测试：1 条 local + 1 条 modbus ── */
    struct sensor_data d_local, d_modbus;
    struct sensor_data mixed_out[2];

    memset(&d_local, 0, sizeof(d_local));
    d_local.temperature = 25.0;
    d_local.humidity = 60.0;
    d_local.pressure = 1000.0;
    d_local.timestamp_ms = 2000;
    assert(storage_save(&d_local, SOURCE_LOCAL, "test-client") == E_OK);

    memset(&d_modbus, 0, sizeof(d_modbus));
    d_modbus.temperature = 30.0;
    d_modbus.humidity = 70.0;
    d_modbus.pressure = 1020.0;
    d_modbus.timestamp_ms = 2001;
    assert(storage_save(&d_modbus, SOURCE_MODBUS, "test-client") == E_OK);

    n = storage_get_pending(mixed_out, 2);
    assert(n == 2);
    assert(mixed_out[0].source == SOURCE_LOCAL);
    assert(mixed_out[1].source == SOURCE_MODBUS);
    assert(mixed_out[0].temperature == 25.0);
    assert(mixed_out[1].temperature == 30.0);

    /* ── 5. legacy 接口仍可用（按时间戳全删）── */
    assert(storage_delete_sent(2001) == E_OK);
    n = storage_get_pending(mixed_out, 2);
    assert(n == 0);

    storage_close();
    printf("storage test passed\n");
    return 0;
}
