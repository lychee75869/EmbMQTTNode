/*
 * subdev_registry.h / subdev_registry.c
 * 子设备注册表（docs/12 §5）：子配置解析 + 校验 + 查询，数据源无关（决策③）。
 *
 * 设计要点：
 *   - 注册表 key 是"数据源键"（slave_id），不感知 libmodbus；未来新增
 *     数据源（如 STM32 BLE）只需新增 subdev_src 枚举值与对应 source_key，
 *     查表/上报/注册代码零改动。
 *   - 解析风格与 config.c 完全一致（INI 风格 key=value，#/; 注释，[段] 跳过）。
 *   - 加载时机：platform_select()（线程创建之前）一次性读入，运行期只读，
 *     无需锁（docs/12 §8）。
 *
 * v1.4.0 网关纯化：移除 SUBDEV_SENSOR 通道（板载采集层删除后无本地
 * 数据来源），sensor 条目落入 unknown data_source 的 WARN+丢弃兜底。
 */
#ifndef SUBDEV_REGISTRY_H
#define SUBDEV_REGISTRY_H

#include "common.h"

/* 数据源种类（预留 SUBDEV_BLE：蓝牙近场子设备演进） */
enum subdev_src {
    SUBDEV_MODBUS = 1,   /* Modbus 从站：source_key = slave_id (1..247) */
};

#define SUBDEVICE_MAX        16     /* 最多 16 个子设备 */
#define SUBDEV_DEVICE_ID_LEN 260    /* 官方 device_id String(256) + 裕量 */

/* 单条子设备注册表项 */
struct subdev_entry {
    enum subdev_src src;                    /* 数据源种类 */
    int             slave_id;               /* MODBUS: 1..247 */
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

/*
 * 只读枚举（T04）：遍历已加载条目而不复制第二份数组。
 * subdev_count: 已加载条目数，0..SUBDEVICE_MAX。
 * subdev_at   : idx∈[0,count) → 条目首地址（指向模块静态 g_entries，只读）；
 *               越界 → NULL。
 */
int subdev_count(void);
const struct subdev_entry *subdev_at(int idx);

/* 按 slave_id 查询 Modbus 子设备条目；未命中返回 NULL */
const struct subdev_entry *subdev_find_modbus(int slave_id);

#endif /* SUBDEV_REGISTRY_H */
