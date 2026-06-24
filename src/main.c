/*
 * main.c
 * 程序入口：初始化、启动采集线程、启动上报线程
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include "common.h"
#include "config.h"
#include "sensor.h"
#include "storage.h"
#include "mqtt_client.h"
#include "daemon.h"

static volatile int g_running = 1;
static struct node_config g_cfg;

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static void *sample_thread(void *arg)
{
    (void)arg;
    struct sensor_data data;

    while (g_running) {
        int rc = sensor_read(&data);
        if (rc != E_OK) {
            LOG_ERROR("sensor_read failed: %d", rc);
            usleep(g_cfg.sample_interval_ms * 1000);
            continue;
        }

        LOG_INFO("sample: temp=%.2f hum=%.2f pres=%.2f",
                 data.temperature, data.humidity, data.pressure);

        if (mqtt_is_connected()) {
            mqtt_publish(&g_cfg, &data);
        } else {
            LOG_INFO("mqtt offline, save to local storage");
            storage_save(&data, g_cfg.client_id);
        }

        usleep(g_cfg.sample_interval_ms * 1000);
    }
    return NULL;
}

static void *upload_thread(void *arg)
{
    (void)arg;
    struct sensor_data pending[16];

    while (g_running) {
        if (mqtt_is_connected()) {
            int n = storage_get_pending(pending, 16);
            if (n > 0) {
                int64_t max_ts = 0;
                for (int i = 0; i < n; i++) {
                    mqtt_publish(&g_cfg, &pending[i]);
                    if (pending[i].timestamp_ms > max_ts)
                        max_ts = pending[i].timestamp_ms;
                }
                storage_delete_sent(max_ts);
            }
        }
        sleep(5);
    }
    return NULL;
}

static void usage(const char *prog)
{
    printf("Usage: %s [-c config] [-d]\n", prog);
    printf("  -c config   指定配置文件路径\n");
    printf("  -d          以守护进程方式运行\n");
}

int main(int argc, char *argv[])
{
    const char *cfg_path = "config/node.conf";
    int daemon_mode = 0;

    int opt;
    while ((opt = getopt(argc, argv, "c:dh")) != -1) {
        switch (opt) {
        case 'c': cfg_path = optarg; break;
        case 'd': daemon_mode = 1; break;
        case 'h':
        default:  usage(argv[0]); return 0;
        }
    }

    if (config_load(cfg_path, &g_cfg) != E_OK) {
        LOG_ERROR("config_load failed");
        return 1;
    }
    config_dump(&g_cfg);

    if (sensor_init(g_cfg.sensor_type) != E_OK) {
        LOG_ERROR("sensor_init failed");
        return 1;
    }

    if (storage_init("data.db") != E_OK) {
        LOG_ERROR("storage_init failed");
        sensor_close();
        return 1;
    }

    if (mqtt_init(g_cfg.broker_host, g_cfg.broker_port, g_cfg.client_id) != E_OK) {
        LOG_ERROR("mqtt_init failed, continue in offline mode");
    }

    if (daemon_mode) {
        daemonize();
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    pthread_t tid_sample, tid_upload;
    pthread_create(&tid_sample, NULL, sample_thread, NULL);
    pthread_create(&tid_upload, NULL, upload_thread, NULL);

    pthread_join(tid_sample, NULL);
    pthread_join(tid_upload, NULL);

    mqtt_close();
    storage_close();
    sensor_close();
    return 0;
}
