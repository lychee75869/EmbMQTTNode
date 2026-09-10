/*
 * mac_addr.h
 * 可测试的 MAC 地址扫描模块（v1.2.5 P1-5 修复）
 *
 * 从 mac_addr.c 抽离 get_mac_address 中的 sysfs 扫描逻辑：
 *   - base_dir 参数化，脱离对真实 /sys/class/net 的依赖
 *   - fclose 收敛为唯一关闭点，结构上杜绝 double fclose
 *   - "lo 兜底"判断改为 ifaces[i+1] == NULL（是否最后一个候选），
 *     消灭硬编码下标，候选列表增删不再腐化
 */
#ifndef MAC_ADDR_H
#define MAC_ADDR_H

#include "common.h"   /* E_OK / E_IO */

/*
 * 在 base_dir 下按 ifaces 给出的优先级扫描 "<iface>/address" 文件，
 * 返回第一个读取成功的 MAC 地址（已去掉末尾换行符）。
 *
 * "lo" 的 00:00:00:00:00:00 无区分度，仅当其位于候选列表末位
 * （其后无其他候选）时才被接受作为兜底。
 *
 * 返回：E_OK 成功；E_IO 未找到任何可用 MAC 或参数非法。
 * 参数：base_dir 形如 "/sys/class/net"；ifaces 为以 NULL 结尾的
 * 接口名列表（调用方拥有列表，本函数不修改、不释放）。
 */
int mac_scan(const char *base_dir, const char *const ifaces[],
             char *mac, int mac_len);

#endif /* MAC_ADDR_H */