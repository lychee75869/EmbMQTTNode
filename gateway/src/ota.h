/*
 * ota.h / ota.c —— A/B 分区 OTA 远程升级
 *
 * 流程：MQTT 升级指令 → 下载固件 → SHA256 + 签名验签 → 装入备用槽 → 切启动槽
 *       → 重启；新固件启动失败达上限则自动切回旧槽。
 *
 * 安全（全部 fail-closed，勿放宽）：
 *   - 公钥未配置 → 升级指令直接拒绝
 *   - .sig 缺失或签名不匹配 → 拒绝安装
 *   - HTTPS 下载强制校验对端证书与主机名
 *   - 回滚保护：新版本连续启动失败达 max 次即切回旧槽
 *
 * 依赖 libcrypto / libssl（SHA256、验签、TLS），Makefile 已加 -lcrypto -lssl
 */
#ifndef OTA_H
#define OTA_H

#include "common.h"

/* 初始化 OTA。current_version 用于状态上报。返回 E_OK 成功 */
int ota_init(const struct ota_config *cfg,
             const char *client_id,
             const char *current_version);

/* 注册 MQTT 发布回调，用于上报 OTA 状态 */
void ota_set_mqtt_publish(int (*publish_cb)(const char *topic,
                                            const char *payload,
                                            int qos));

/* 处理收到的 OTA MQTT 消息 */
void ota_handle_message(const char *payload, int payload_len);

/* 周期性驱动 OTA 状态机（主循环调用）。返回 1=正在处理，0=空闲 */
int ota_check_and_handle(void);

/*
 * 启动后健康检查（本地机制，不依赖 MQTT/网络），须在 ota_init() 与
 * ota_set_mqtt_publish() 之后调用：
 *   boot_count == 0        已确认健康，正常启动
 *   0 < boot_count < max   递增计数，继续试用
 *   boot_count >= max      试用期反复失败 → 切回旧槽并 exit(42)
 * 下行指令路由由平台层唯一持有（mqtt_client.on_message →
 * platform_dispatch_message → platform_local.on_message → ota_handle_message），
 * 无需再注册回调。
 */
void ota_post_boot_check(void);

/* 固件稳定运行 boot_confirm_sec 秒后调用：boot_count 清零并上报 confirmed */
void ota_confirm_boot(void);

/* 当前 OTA 状态的字符串描述 */
const char *ota_state_string(void);

/*
 * 固件签名验签（独立可测）：用 pubkey_path 的 PEM 公钥（RSA/EC → SHA256
 * 摘要签名，Ed25519 → 原生签名）验证 sig_path 对 fw_path 的签名
 * （签名工具 tools/sign_firmware.sh）。
 * 返回 E_OK 通过；E_INVAL/E_IO/E_NOT_FOUND 参数或文件问题；其他非 0 签名不匹配
 */
int ota_verify_signature(const char *fw_path,
                         const char *sig_path,
                         const char *pubkey_path);

/* 关闭 OTA 子系统 */
void ota_close(void);

#endif /* OTA_H */
