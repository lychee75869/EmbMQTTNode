/*
 * config.h / config.c
 * 配置文件解析模块
 * 支持 INI 风格键值对，格式：key = value
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

/*
 * 加载配置文件（INI 风格键值对）
 * path: 配置文件路径
 * cfg:  输出配置结构体（先填默认值，再覆盖）
 * 返回: E_OK 成功，E_IO 文件打开失败
 */
int config_load(const char *path, struct node_config *cfg);

/*
 * 打印当前配置到日志（LOG_INFO）
 */
void config_dump(const struct node_config *cfg);

#endif /* CONFIG_H */
