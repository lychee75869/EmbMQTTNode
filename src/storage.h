/*
 * storage.h / storage.c
 * 本地 SQLite 缓存，支持断网续传
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "common.h"

/* 初始化数据库 */
int storage_init(const char *db_path);

/* 保存一条传感器数据 */
int storage_save(const struct sensor_data *data, const char *client_id);

/* 获取 count 条待发送数据，返回实际条数 */
int storage_get_pending(struct sensor_data *out, int count);

/* 删除指定时间戳之前（含）的数据 */
int storage_delete_sent(int64_t timestamp_ms);

/* 关闭数据库 */
void storage_close(void);

#endif /* STORAGE_H */
