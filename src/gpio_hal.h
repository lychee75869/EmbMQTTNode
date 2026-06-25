/*
 * gpio_hal.h / gpio_hal.c
 * GPIO 硬件抽象层（阶段三）
 *
 * 支持两种模式：
 *   - libgpiod 真实硬件模式（Linux, CONFIG_GPIO=y）
 *   - mock 模式（开发调试，仅打印日志）
 *
 * 默认使用 mock 模式，设置 GPIO_REAL_HW=1 启用真实 GPIO。
 */
#ifndef GPIO_HAL_H
#define GPIO_HAL_H

#include "common.h"

/*
 * 初始化 GPIO 子系统
 * 返回: E_OK 成功
 */
int gpio_hal_init(void);

/*
 * 设置 GPIO 引脚输出电平
 * pin:   引脚编号 (1..GPIO_PIN_MAX)
 * value: 0=低电平, 1=高电平
 * 返回: E_OK 成功，E_INVAL 参数无效
 */
int gpio_hal_set(int pin, int value);

/*
 * 关闭 GPIO 子系统，释放所有引脚
 */
void gpio_hal_close(void);

#endif /* GPIO_HAL_H */
