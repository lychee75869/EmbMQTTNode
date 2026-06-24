/*
 * config.h / config.c
 * 配置文件解析模块
 * 支持 INI 风格键值对，格式：key = value
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "common.h"

int config_load(const char *path, struct node_config *cfg);
void config_dump(const struct node_config *cfg);

#endif /* CONFIG_H */
