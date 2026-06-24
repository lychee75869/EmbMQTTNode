/*
 * tests/test_storage.c
 * 存储模块单元测试
 */
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../src/storage.h"

int main(void)
{
    struct sensor_data data[3];
    struct sensor_data out[3];

    /* 清理历史测试数据 */
    remove("test_data.db");
    assert(storage_init("test_data.db") == E_OK);

    for (int i = 0; i < 3; i++) {
        data[i].temperature = 20.0 + i;
        data[i].humidity = 50.0 + i;
        data[i].pressure = 1010.0 + i;
        data[i].timestamp_ms = 1000 + i;
        assert(storage_save(&data[i], "test-client") == E_OK);
    }

    int n = storage_get_pending(out, 3);
    assert(n == 3);
    assert(out[0].timestamp_ms == 1000);
    assert(out[2].timestamp_ms == 1002);

    assert(storage_delete_sent(1002) == E_OK);
    n = storage_get_pending(out, 3);
    assert(n == 0);

    storage_close();
    printf("storage test passed\n");
    return 0;
}
