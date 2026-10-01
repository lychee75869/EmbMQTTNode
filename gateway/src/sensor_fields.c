/*
 * sensor_fields.c
 * 传感器字段描述表定义与查表存取。
 *
 * 【表序不变式（严禁破坏）】
 *   SENSOR_FIELDS[] 顺序 == payload 序列化序
 *   （temperature, humidity, pressure, soil_moisture, water_level,
 *     battery_voltage）。序列化侧（mqtt_client / http_server）按本表顺序
 *   遍历输出，因此表序即 payload 序：变更表序会破坏 local 路径逐字节基线。
 *   新增字段只许追加表尾（append-only），严禁插入/重排。
 */
#include "sensor_fields.h"

/* 表内容：顺序即 payload 序（冻结不变式）。
 * 表尾追加 soil_moisture / water_level / battery_voltage，
 * 对齐 8 寄存器子设备契约。 */
const struct sensor_field SENSOR_FIELDS[SENSOR_FIELD_COUNT] = {
    { "temperature",     offsetof(struct sensor_data, temperature),     SF_F64 },
    { "humidity",        offsetof(struct sensor_data, humidity),        SF_F64 },
    { "pressure",        offsetof(struct sensor_data, pressure),        SF_F64 },
    { "soil_moisture",   offsetof(struct sensor_data, soil_moisture),   SF_F64 },
    { "water_level",     offsetof(struct sensor_data, water_level),     SF_F64 },
    { "battery_voltage", offsetof(struct sensor_data, battery_voltage), SF_F64 },
};

/* 守卫：SENSOR_FIELD_COUNT 必须与表实际尺寸一致（防止改表漏改常量，
 * 或反之导致遍历越界）。 */
_Static_assert(sizeof(SENSOR_FIELDS) / sizeof(SENSOR_FIELDS[0]) ==
                   SENSOR_FIELD_COUNT,
               "SENSOR_FIELD_COUNT must match SENSOR_FIELDS table size");

const struct sensor_field *sensor_find_field(const char *name)
{
    if (!name)
        return NULL;
    for (int i = 0; i < SENSOR_FIELD_COUNT; i++) {
        if (strcmp(SENSOR_FIELDS[i].name, name) == 0)
            return &SENSOR_FIELDS[i];
    }
    return NULL;
}

double sensor_get_field(const struct sensor_data *d, const char *name)
{
    const struct sensor_field *f = sensor_find_field(name);
    if (!d || !f)
        return SENSOR_VALUE_INVALID;   /* 未知字段，永不为真（引擎语义兼容） */
    return *(const double *)((const char *)d + f->offset);
}

int sensor_set_field(struct sensor_data *d, const char *name, double v)
{
    const struct sensor_field *f = sensor_find_field(name);
    if (!d || !f)
        return E_INVAL;                /* 未知字段：调用方 WARN（沿用旧行为） */
    *(double *)((char *)d + f->offset) = v;
    return E_OK;
}
