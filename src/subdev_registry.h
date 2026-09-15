/*
 * subdev_registry.h / subdev_registry.c
 * 子设备注册表（docs/12 §5）：子配置解析 + 校验 + 查询，数据源无关（决策③）。
 *
 * 设计要点：
 *   - 注册表 key 是"数据源键"（sensor_type / slave_id），不感知 SHT30 驱动
 *     或 libmodbus；未来新增数据源（如 STM32 BLE）只需新增 subdev_src 枚举
 *     值与对应 source_key，查表/上报/注册代码零改动。
 *   - 解析风格与 config.c 完全一致（INI 风格 key=value，#/; 注释，[段] 跳过）。
 *   - 加载时机：platform_select()（线程创建之前）一次性读入，运行期只读，
 *     无需锁（docs/12 §8）。
 */
#ifndef SUBDEV_REGISTRY_H
#define SUBDEV_REGISTRY_H

#include "common.h"

/* 数据源种类（预留 SUBDEV_BLE：蓝牙近场子设备演进） */
enum subdev_src {
    SUBDEV_SENSOR = 0,   /* 本地传感器：source_key = sensor_type */
    SUBDEV_MODBUS = 1,   /* Modbus 从站：source_key = slave_id (1..247) */
};

#define SUBDEVICE_MAX        16     /* 最多 16 个子设备 */
#define SUBDEV_DEVICE_ID_LEN 260    /* 官方 device_id String(256) + 裕量 */

/* 单条子设备注册表项 */
struct subdev_entry {
    enum subdev_src src;                    /* 数据源种类 */
    int             slave_id;               /* MODBUS: 1..247；SENSOR 恒 0 */
    char            sensor_type[32];        /* SENSOR: sht30/ads1115/mock；MODBUS 恒 "" */
    char            device_id[SUBDEV_DEVICE_ID_LEN];  /* 平台分配的子设备 device_id */
    char            name[64];               /* 展示名（日志用） */
    char            service_id[32];         /* 缺省 "SensorData"（与云侧 C-1 对齐） */
};

/*
 * 加载子设备注册表（纯函数式解析，可单测）。
 * path: 子配置路径；文件不存在/0 条 → WARN + 空表（不 crash）。
 * out:  可选输出数组（可 NULL）；按 max 与 SUBDEVICE_MAX 较小者拷贝。
 * max:  out 数组容量。
 * 返回: 成功加载的条目数（已做全部 fail-closed 校验）。
 */
int subdev_load(const char *path, struct subdev_entry *out, int max);

/* 按 sensor_type 查询本地传感器子设备条目；未命中返回 NULL */
const struct subdev_entry *subdev_find_sensor(const char *sensor_type);

/* 按 slave_id 查询 Modbus 子设备条目；未命中返回 NULL */
const struct subdev_entry *subdev_find_modbus(int slave_id);

#endif /* SUBDEV_REGISTRY_H */
