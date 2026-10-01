/*
 * sensor_fields.h / sensor_fields.c —— 传感器字段描述表（"字段名 ↔ 值"单一事实源）
 *
 * 消费侧（序列化 / 取值 / 上报 / dashboard）统一查表或遍历，不再各写 strcmp 链；
 * 新增字段退化为「struct +1 成员、表 +1 项、生产侧采集、storage 列迁移」4 处改动，
 * 消费侧零改动。
 *
 * 禁止升级为动态 key-value / EAV / JSON blob：破坏类型安全、引入堆分配、
 * get_field 由 O(1) 退化为线性、storage 固定列语义丢失；字段数个位数，
 * 具名 struct + 一张描述表是甜点区。
 */
#ifndef SENSOR_FIELDS_H
#define SENSOR_FIELDS_H

#include "common.h"
#include <stddef.h>

/*
 * 字段表条目数（编译期常量）。
 *
 * 【表序不变式】SENSOR_FIELDS 的数组序 == payload 序列化序
 * （temperature, humidity, pressure, soil_moisture, water_level, battery_voltage）。
 * 变更表序即破坏 local 路径逐字节基线——新增字段只许【追加表尾】，
 * 严禁插入/重排。
 *
 * 消费侧 TU 只可见 extern 不完全数组类型，无法对其求 sizeof，故此处显式给出
 * 常量，并在 sensor_fields.c 内以 _Static_assert 守卫其与表实际尺寸一致。
 */
#define SENSOR_FIELD_COUNT 6

enum sensor_field_type {
    SF_F64 = 0,   /* double 字段；预留 SF_I64 / SF_STR */
};

struct sensor_field {
    const char             *name;   /* 字段名（新增字段只许追加表尾） */
    size_t                  offset; /* offsetof(struct sensor_data, x) */
    enum sensor_field_type  type;
};

extern const struct sensor_field SENSOR_FIELDS[SENSOR_FIELD_COUNT];

/* 供序列化侧遍历：f 为 const struct sensor_field *（自动声明于 for 初始化） */
#define for_each_field(f) \
    for (const struct sensor_field *f = SENSOR_FIELDS; \
         f < SENSOR_FIELDS + SENSOR_FIELD_COUNT; f++)

/* 查表：命中返回条目指针；未命中（含 NULL）返回 NULL */
const struct sensor_field *sensor_find_field(const char *name);

/* 取值：命中返回字段值；未知字段或 d==NULL 返回 SENSOR_VALUE_INVALID */
double sensor_get_field(const struct sensor_data *d, const char *name);

/* 设值：命中写入并返回 E_OK；未知字段或 d==NULL 返回 E_INVAL（调用方 WARN） */
int sensor_set_field(struct sensor_data *d, const char *name, double v);

#endif /* SENSOR_FIELDS_H */
