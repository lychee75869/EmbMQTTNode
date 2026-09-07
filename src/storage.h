/*
 * storage.h / storage.c
 * 本地 SQLite 缓存，支持断网续传
 */
#ifndef STORAGE_H
#define STORAGE_H

#include "common.h"

/* 初始化数据库（含旧库 source 列 migration） */
int storage_init(const char *db_path);

/* 保存一条传感器数据，source 指明来源（local/modbus）；成功后回填 data->id */
int storage_save(const struct sensor_data *data, sensor_source_t source,
                 const char *client_id);

/* 获取 count 条待发送数据（含 id + source），返回实际条数 */
int storage_get_pending(struct sensor_data *out, int count);

/* 新增：按主键精确删除单条（main.c 补发成功后逐条调用） */
int storage_delete_by_id(int64_t id);

/*
 * legacy：按时间戳删除（timestamp_ms <= 该值全部删除）
 * main.c 不再使用（时间戳删除会误删发布失败的数据），仅供测试或
 * 全表清理等管理命令调用
 */
int storage_delete_sent(int64_t timestamp_ms);

/* 关闭数据库 */
void storage_close(void);

#endif /* STORAGE_H */
