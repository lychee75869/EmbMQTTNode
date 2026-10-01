/*
 * mac_addr.h / mac_addr.c —— 可测试的 MAC 地址扫描
 *
 * 从 get_mac_address 抽离 sysfs 扫描逻辑：base_dir 参数化（脱离对真实
 * /sys/class/net 的依赖）；fclose 收敛为唯一关闭点，结构上杜绝 double fclose；
 * "lo 兜底"判断改为依据是否最后一个候选，不再依赖硬编码下标。
 */
#ifndef MAC_ADDR_H
#define MAC_ADDR_H

#include "common.h"   /* E_OK / E_IO */

/*
 * 在 base_dir 下按 ifaces 给出的优先级扫描 "<iface>/address"，返回第一个读取
 * 成功的 MAC（已去掉末尾换行）。"lo" 的 00:00:00:00:00:00 无区分度，仅当它
 * 位于候选列表末位时才作为兜底被接受。
 *
 * base_dir 形如 "/sys/class/net"；ifaces 为以 NULL 结尾的接口名列表
 * （调用方拥有，本函数不修改、不释放）。
 * 返回 E_OK 成功；E_IO 未找到可用 MAC 或参数非法。
 */
int mac_scan(const char *base_dir, const char *const ifaces[],
             char *mac, int mac_len);

#endif /* MAC_ADDR_H */
