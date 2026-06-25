/*
 * rule_engine.h / rule_engine.c
 * 规则引擎 + 本地告警模块（阶段三）
 *
 * 从配置文件加载阈值/趋势/窗口规则，对传感器数据实时评估，
 * 触发时返回动作掩码，由 main 线程执行 MQTT 告警 / GPIO 控制。
 */
#ifndef RULE_ENGINE_H
#define RULE_ENGINE_H

#include "common.h"

/*
 * 初始化规则引擎
 * cfg: 节点配置（从中读取 rule_count + rules[] 数组）
 * 返回: E_OK 成功，E_INVAL 参数无效
 */
int rule_engine_init(const struct node_config *cfg);

/*
 * 评估一条传感器数据，对所有已加载规则进行匹配
 * data:        传感器数据
 * alert_msg:   输出缓冲区（告警描述），可为 NULL
 * alert_msg_len: 缓冲区长度
 * 返回: 触发的动作位掩码（OR 组合），0 表示无规则触发
 */
uint8_t rule_engine_evaluate(const struct sensor_data *data,
                              char *alert_msg, int alert_msg_len);

/*
 * 获取规则统计信息
 * stats:     输出数组
 * max_count: 数组容量
 * 返回: 实际填充的统计条数
 */
int rule_engine_get_stats(struct rule_stats *stats, int max_count);

/*
 * 关闭规则引擎，释放资源
 */
void rule_engine_close(void);

#endif /* RULE_ENGINE_H */
