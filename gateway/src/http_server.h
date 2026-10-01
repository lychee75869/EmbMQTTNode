/*
 * http_server.h / http_server.c —— 本地 Web Dashboard
 * 最小化 HTTP/1.0 服务器，纯 POSIX socket、零外部依赖，在独立线程中运行。
 * 路由：
 *   GET  /                        内嵌 HTML 仪表盘（单页应用）
 *   GET  /api/status              设备状态 JSON
 *   GET  /api/data/latest         最新传感器读数
 *   GET  /api/data/history?n=N    最近 N 条记录（来自 SQLite）
 *   GET  /api/ota/status          OTA 升级状态
 *   POST /api/reboot              触发重启（需 token 认证）
 */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "common.h"

/*
 * 启动 HTTP 服务器（阻塞，在调用线程中跑 accept 循环）
 * bind_addr 为 NULL 表示 0.0.0.0；dev/cfg 只读
 */
int http_server_start(int port, const char *bind_addr,
                      const struct device_info *dev,
                      const struct node_config *cfg);

/* 通知 HTTP 服务器停止（从其他线程调用） */
void http_server_stop(void);

/* 更新最新传感器数据（从采集线程调用，线程安全） */
void http_server_update_data(const struct sensor_data *data);

/*
 * 常量时间字符串比较，供 /api/reboot 的 token 认证：SHA256 摘要 +
 * CRYPTO_memcmp，比较耗时与内容无关（防时序侧信道）。返回 1 相等 / 0 不相等
 * （任一参数为 NULL 或摘要失败均返回 0）。单独导出供单元测试使用。
 */
int http_consttime_token_equal(const char *a, const char *b);

#endif /* HTTP_SERVER_H */
