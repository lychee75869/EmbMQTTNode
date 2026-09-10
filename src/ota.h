/*
 * ota.h / ota.c
 * A/B 分区 OTA 远程升级模块（阶段四）
 *
 * 功能：
 *   - MQTT 接收升级指令 → 下载固件 → SHA256 校验 →
 *     安装到备用槽位 → 切换启动槽 → 重启
 *   - A/B 分区（开发阶段用目录模拟）
 *   - 启动失败自动回滚（boot attempt 计数）
 *   - 升级进度/状态通过 MQTT 上报
 *
 * 安全：
 *   - SHA256 固件完整性校验
 *   - 回滚保护：新版本启动失败 3 次自动切回旧版本
 *
 * 依赖：libcrypto (OpenSSL) 用于 SHA256
 *       make 时自动添加 -lcrypto
 */
#ifndef OTA_H
#define OTA_H

#include "common.h"

/*
 * 初始化 OTA 子系统
 * cfg:             OTA 配置
 * client_id:       设备 client_id（用于 MQTT topic）
 * current_version: 当前固件版本号
 * 返回: E_OK 成功
 */
int ota_init(const struct ota_config *cfg,
             const char *client_id,
             const char *current_version);

/*
 * 注册 MQTT 发布回调（用于上报 OTA 状态）
 * publish_cb: 函数指针，签名: int publish(const char *topic, const char *payload, int qos)
 */
void ota_set_mqtt_publish(int (*publish_cb)(const char *topic,
                                             const char *payload,
                                             int qos));

/*
 * 处理收到的 OTA MQTT 消息
 * payload:      JSON 消息体
 * payload_len: 消息长度
 */
void ota_handle_message(const char *payload, int payload_len);

/*
 * 周期性驱动 OTA 状态机（在 main 循环中调用，例如每 5 秒一次）
 * 返回: 0=空闲, 1=正在处理中
 */
int ota_check_and_handle(void);

/*
 * 启动后健康检查（不依赖 MQTT/网络，本地安全机制）：
 *   - boot_count == 0：已确认健康，正常启动
 *   - 0 < boot_count < max：递增计数，继续试用
 *   - boot_count >= max：试用期内反复失败 → 切回旧槽，exit(42)
 * 必须在 ota_init() 与 ota_set_mqtt_publish() 之后、mqtt_set_ota_callback()
 */
void ota_post_boot_check(void);

/*
 * 启动健康确认（周期驱动）：固件稳定运行 boot_confirm_sec 秒后由主循环调用，
 * 将 boot_count 清零并上报 confirmed。已确认（count==0）或未启用时为 no-op。
 */
void ota_confirm_boot(void);

/*
 * 返回当前 OTA 状态的字符串描述
 */
const char *ota_state_string(void);

/*
 * 关闭 OTA 子系统
 */
void ota_close(void);

#endif /* OTA_H */
