/*
 * http_server.h / http_server.c
 * 本地 Web Dashboard HTTP 服务器（阶段五）
 *
 * 最小化 HTTP/1.0 服务器，零外部依赖（纯 POSIX socket）。
 * 在独立线程中运行，提供：
 *   - GET  /              内嵌 HTML 仪表盘（单页应用）
 *   - GET  /api/status    设备状态 JSON
 *   - GET  /api/data/latest  最新传感器读数
 *   - GET  /api/data/history?n=N  最近 N 条记录（from SQLite）
 *   - GET  /api/rules     规则引擎统计
 *   - GET  /api/anomaly   异常检测引擎统计
 *   - GET  /api/ota/status    OTA 升级状态
 *   - POST /api/reboot    触发重启（需 token 认证）
 */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "common.h"

/*
 * 启动 HTTP 服务器（阻塞，在调用线程中运行 accept 循环）
 * port:        监听端口（如 8080）
 * bind_addr:   绑定地址（NULL = 0.0.0.0）
 * dev:         设备信息指针（只读，供 /api/status 使用）
 * cfg:         节点配置指针（只读）
 * 返回:        E_OK 正常退出，其他为错误
 */
int http_server_start(int port, const char *bind_addr,
                      const struct device_info *dev,
                      const struct node_config *cfg);

/*
 * 通知 HTTP 服务器停止（从其他线程调用）
 */
void http_server_stop(void);

/*
 * 更新最新传感器数据（从采集线程调用，线程安全）
 */
void http_server_update_data(const struct sensor_data *data);

/*
 * P1-6（v1.2.9）: 常量时间字符串比较，供 /api/reboot token 认证。
 * SHA256 摘要 + CRYPTO_memcmp，比较耗时与内容无关（防时序侧信道）。
 * 返回 1 相等 / 0 不相等（任一参数 NULL、摘要失败均返回 0）。
 * 单独导出供单元测试（tests/test_mqtt_client.c）。
 */
int http_consttime_token_equal(const char *a, const char *b);

#endif /* HTTP_SERVER_H */
