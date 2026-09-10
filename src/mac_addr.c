/*
 * mac_addr.c
 * 可测试的 MAC 地址扫描模块（v1.2.5 P1-5 修复）
 *
 * 见 mac_addr.h 顶部注释了解背景。本实现独立于 main.c，可被单元
 * 测试通过构造临时 sysfs 树直接覆盖（旧实现在 main.c 里 static
 * 且 tests/Makefile 直链 main.c 失败——main() 冲突）。
 */
#include <stdio.h>
#include <string.h>

#include "mac_addr.h"
#include "common.h"   /* E_OK / E_IO */

int mac_scan(const char *base_dir, const char *const ifaces[],
             char *mac, int mac_len) {
    char path[256];

    if (!base_dir || !ifaces || !mac || mac_len <= 0)
        return E_IO;

    for (int i = 0; ifaces[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s/address", base_dir, ifaces[i]);

        /* fopen/fclose 配对收敛：先尝试读，再唯一关闭点关闭。
         * 旧实现里 fgets 成功分支先 fclose 再 return（lo 路径还会再
         * fclose 一次）= double fclose（UB，glibc 下通常 abort）。 */
        FILE *fp = fopen(path, "r");
        if (!fp)
            continue;

        int got = (fgets(mac, mac_len, fp) != NULL);
        fclose(fp);   /* 唯一关闭点：结构上杜绝 double fclose */

        if (!got)
            continue;

        /* 去掉末尾换行符 */
        size_t len = strlen(mac);
        if (len > 0 && mac[len - 1] == '\n')
            mac[len - 1] = '\0';

        /* lo 仅当为最后一个候选（其后无候选）时才接受，避免硬编码下标。
         *
         * 越界安全性：进入本循环体时 ifaces[i] != NULL，i 必严格小于
         * NULL 终止位下标，因此 i+1 至多指向 NULL 哨兵，绝不会越界。 */
        if (strcmp(ifaces[i], "lo") != 0 || ifaces[i + 1] == NULL)
            return E_OK;
    }
    return E_IO;
}