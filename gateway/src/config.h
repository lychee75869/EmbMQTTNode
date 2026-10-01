/*
 * config.h / config.c —— 配置文件解析（INI 风格 key = value）
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

/*
 * 加载配置文件：cfg 先填默认值，再被文件内容覆盖。
 * 返回 E_OK 成功，E_IO 文件打开失败
 */
int config_load(const char *path, struct node_config *cfg);

/* 打印当前配置到日志（LOG_INFO） */
void config_dump(const struct node_config *cfg);

#endif /* CONFIG_H */
