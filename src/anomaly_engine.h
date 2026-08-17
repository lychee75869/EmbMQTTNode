/*
 * anomaly_engine.h / anomaly_engine.c
 * 异常检测引擎模块（方向 B）
 *
 * 分层架构：
 *   - 基础层：Z-score 滑动窗口统计（零训练，即开即用）
 *   - 进阶层：Isolation Forest 机器学习（Python 训练 → C const 数组 → 编译进二进制）
 *
 * 功能：
 *   - 从配置文件加载异常检测规则
 *   - 对传感器数据实时评估，输出异常分数 + 动作掩码
 *   - 冷却时间防抖，统计数据收集（供 Web Dashboard 查询）
 *   - 与 rule_engine 独立并行运行，共用 ACTION_* 位掩码
 */
#ifndef ANOMALY_ENGINE_H
#define ANOMALY_ENGINE_H

#include "common.h"

/*
 * 初始化异常检测引擎
 * cfg: 节点配置（从中读取 anomaly_enabled + anoms[] 数组）
 * 返回: E_OK 成功，E_INVAL 参数无效
 */
int anomaly_engine_init(const struct node_config *cfg);

/*
 * 评估一条传感器数据，对所有已加载异常规则进行检测
 * data:         传感器数据
 * alert_msg:    输出缓冲区（告警描述），可为 NULL
 * alert_msg_len: 缓冲区长度
 * 返回: 触发的动作位掩码（OR 组合），0 表示无异常
 */
uint8_t anomaly_engine_evaluate(const struct sensor_data *data,
                                 char *alert_msg, int alert_msg_len);

/*
 * 获取异常检测统计信息
 * stats:     输出数组
 * max_count: 数组容量
 * 返回: 实际填充的统计条数
 */
int anomaly_engine_get_stats(struct anomaly_stats *stats, int max_count);

/*
 * 关闭异常检测引擎，释放资源
 */
void anomaly_engine_close(void);

#endif /* ANOMALY_ENGINE_H */
