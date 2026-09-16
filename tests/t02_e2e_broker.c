/*
 * tests/t02_e2e_broker.c
 * T02 验收项③「断连 → 自动重连 → 重订 OTA → 断网补发」真实 broker 端到端
 * 验证挂钩（**手动集成**，不进入 make test / TESTS 单元套件——依赖真实
 * mosquitto 与网络时序，不适合作为 CI 单测）。
 *
 * 与生产 main.c 的差别仅在"外壳"：本挂钩不做设备信息采集/HTTP/OTA 状态机，
 * 直接复用生产代码路径：
 *   platform_set_device_info + platform_select + mqtt_init(&cfg)
 *   → CONNACK → platform_on_connected → platform_local.on_connected
 *     （重订 OTA 主题 + 重发 online）
 *   → 断网时 storage_save 入队；重连后 storage_get_pending +
 *     platform_publish_data + storage_delete_by_id（等价 upload_thread 补发）
 *
 * 用法（见 t02_e2e_broker.sh）：编译后运行，外部脚本负责 kill/重启 broker
 * 制造断连；挂钩把关键节点打到 stdout 供断言。
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/common.h"
#include "../src/platform.h"
#include "../src/mqtt_client.h"
#include "../src/storage.h"

#define E2E_BROKER_PORT   18830
#define E2E_TOTAL_TICKS   40          /* 40 x 500ms = 20s 观察窗 */
#define E2E_DB_PATH       "/tmp/embmqttnode_e2e.db"

static struct node_config g_cfg;
static struct device_info g_dev;

static void build_cfg(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    snprintf(g_cfg.broker_host, sizeof(g_cfg.broker_host), "127.0.0.1");
    g_cfg.broker_port = E2E_BROKER_PORT;
    snprintf(g_cfg.topic, sizeof(g_cfg.topic), "embmqttnode/emb-node-e2e");
    snprintf(g_cfg.client_id, sizeof(g_cfg.client_id), "emb-node-e2e");
    /* platform 缺省空串 → local；TLS 关（测试 broker 无 TLS） */
    g_cfg.tls.enabled = 0;
    g_cfg.ota.enabled = 0;   /* 下行路由由 platform_local 唯一持有，无关 enabled */

    memset(&g_dev, 0, sizeof(g_dev));
    snprintf(g_dev.hostname, sizeof(g_dev.hostname), "e2ehost");
    snprintf(g_dev.mac_addr, sizeof(g_dev.mac_addr), "aa:bb:cc:dd:ee:ff");
    snprintf(g_dev.mac_short, sizeof(g_dev.mac_short), "aabbcc");
    snprintf(g_dev.cpu_model, sizeof(g_dev.cpu_model), "e2ecpu");
    snprintf(g_dev.kernel_ver, sizeof(g_dev.kernel_ver), "6.6.0-e2e");
    g_dev.total_mem_kb = 1024;
}

int main(void)
{
    remove(E2E_DB_PATH);
    build_cfg();

    if (storage_init(E2E_DB_PATH) != E_OK) {
        fprintf(stderr, "E2E: storage_init failed\n");
        return 2;
    }

    platform_set_device_info(&g_dev);
    platform_select(&g_cfg);

    if (mqtt_init(&g_cfg) != E_OK) {
        fprintf(stderr, "E2E: mqtt_init failed\n");
        storage_close();
        return 2;
    }

    printf("E2E: harness started, watching for %d ticks\n", E2E_TOTAL_TICKS);
    fflush(stdout);

    int saved_offline = 0;
    int seen_connects = 0;
    int prev_connected = 0;

    for (int i = 0; i < E2E_TOTAL_TICKS; i++) {
        int conn = mqtt_is_connected();

        if (conn && !prev_connected) {
            seen_connects++;
            printf("E2E: CONNECTED #%d at tick %d\n", seen_connects, i);
            fflush(stdout);
        }
        prev_connected = conn;

        if (conn) {
            if (saved_offline) {
                /* 重连后补发（等价 upload_thread）：经平台路径发布 → 删除 */
                struct sensor_data pending[16];
                int n = storage_get_pending(pending, 16);
                for (int k = 0; k < n; k++) {
                    if (platform_publish_data(&g_cfg, &pending[k]) == E_OK) {
                        storage_delete_by_id(pending[k].id);
                        printf("E2E: REPLAYED pending id=%lld on tick %d\n",
                               (long long)pending[k].id, i);
                        fflush(stdout);
                    }
                }
                saved_offline = 0;
            }
        } else {
            if (!saved_offline) {
                struct sensor_data d;
                memset(&d, 0, sizeof(d));
                d.temperature = 12.34;
                d.humidity = 56.78;
                d.pressure = 1013.25;
                d.timestamp_ms = (int64_t)time(NULL) * 1000LL;
                d.source = SOURCE_LOCAL;
                if (storage_save(&d, SOURCE_LOCAL, g_cfg.client_id) == E_OK) {
                    printf("E2E: SAVED offline sample at tick %d\n", i);
                    fflush(stdout);
                    saved_offline = 1;
                }
            }
        }

        usleep(500000);
    }

    printf("E2E: harness done, total connects=%d\n", seen_connects);
    fflush(stdout);

    mqtt_close();
    storage_close();
    remove(E2E_DB_PATH);
    return 0;
}
