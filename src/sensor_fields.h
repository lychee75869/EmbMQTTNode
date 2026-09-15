/*
 * sensor_fields.h / sensor_fields.c
 * 传感器字段描述表（单一事实源，docs/12 §3.4）。
 *
 * 目标：把「字段名 ↔ 值」映射收敛为一处；消费侧（序列化 / 取值 / 上报 /
 * dashboard）统一查表/遍历，不再各写 strcmp 链。新增字段（如 CO2）退化为
 * 「struct +1 成员、表 +1 项、生产侧采集、storage 列迁移」4 处改动，
 * 消费侧零改动。
 *
 * 明确禁止升级为动态 key-value / EAV / JSON blob：破坏类型安全、引入堆分配、
 * get_field 由 O(1) 退化为线性、storage 固定列语义丢失；字段数个位数，
 * 具名 struct + 一张描述表是甜点区（docs/12 §3.4 / PRD P1-4）。
 */
#ifndef SENSOR_FIELDS_H
#define SENSOR_FIELDS_H

#include "common.h"
#include <stddef.h>

/*
 * 字段表条目数（编译期常量）。
 *
 * 【表序不变式】SENSOR_FIELDS 的数组序 == v1.2.11 payload 序列化序
 * （temperature, humidity, pressure）。变更表序即破坏 local 路径逐字节
 * 基线——新增字段只许【追加表尾】，严禁插入/重排。
 *
 * 说明：消费侧 TU 只可见 `extern` 不完全数组类型，无法对之求
 * sizeof(SENSOR_FIELDS)（C 语言 sizeof 需完整类型），故此处显式给出
 * 常量，并在 sensor_fields.c 内以 _Static_assert 守卫其与表实际尺寸一致
 * （与设计 §3.4.1 的 sizeof 宏语义等价）。
 */
#define SENSOR_FIELD_COUNT 3

enum sensor_field_type {
    SF_F64 = 0,   /* double 字段；预留 SF_I64 / SF_STR */
};

struct sensor_field {
    const char             *name;   /* "temperature" / "humidity" / "pressure" */
    size_t                  offset; /* offsetof(struct sensor_data, x) */
    enum sensor_field_type  type;
};

extern const struct sensor_field SENSOR_FIELDS[SENSOR_FIELD_COUNT];

/* 供序列化侧遍历：f 为 const struct sensor_field *（自动声明于 for 初始化） */
#define for_each_field(f) \
    for (const struct sensor_field *f = SENSOR_FIELDS; \
         f < SENSOR_FIELDS + SENSOR_FIELD_COUNT; f++)

/*
 * 查表：命中返回条目指针；未命中（含 NULL）返回 NULL。
 * 供 config 加载时校验 rule_N / anomaly_N 的 field 名（fail-closed）。
 */
const struct sensor_field *sensor_find_field(const char *name);

/*
 * 取值：命中返回 *(double*)((char*)d + offset)；
 * 未知字段（或 d==NULL）返回 SENSOR_VALUE_INVALID（与引擎"永不匹配"语义兼容）。
 */
double sensor_get_field(const struct sensor_data *d, const char *name);

/*
 * 设值：命中写入并返回 E_OK；
 * 未知字段（或 d==NULL）返回 E_INVAL（沿用现 set_field 行为，调用方 WARN）。
 */
int sensor_set_field(struct sensor_data *d, const char *name, double v);

#endif /* SENSOR_FIELDS_H */
