/*
 * gpio_hal.c
 * GPIO 硬件抽象层实现（阶段三）
 *
 * 默认 mock 模式：所有 GPIO 操作仅打印日志。
 * 编译时定义 GPIO_REAL_HW=1 启用 libgpiod 真实硬件控制。
 *   make GPIO_REAL_HW=1  → 链接 libgpiod
 */

#include "gpio_hal.h"

#ifdef GPIO_REAL_HW
#include <gpiod.h>
#endif

/* ─── 内部状态 ─────────────────────────────────────────── */

static int g_gpio_initialized = 0;

#ifdef GPIO_REAL_HW
static struct gpiod_chip    *g_chip = NULL;
static struct gpiod_line    *g_lines[GPIO_PIN_MAX + 1] = { NULL };
#endif

/* ─── Mock 模式 ─────────────────────────────────────────── */

#ifndef GPIO_REAL_HW

int gpio_hal_init(void)
{
    g_gpio_initialized = 1;
    LOG_INFO("gpio hal init: mock mode (GPIO_REAL_HW=0)");
    LOG_INFO("gpio pins 1..%d available (log-only)", GPIO_PIN_MAX);
    return E_OK;
}

int gpio_hal_set(int pin, int value)
{
    if (!g_gpio_initialized) return E_INVAL;
    if (pin < 1 || pin > GPIO_PIN_MAX) {
        LOG_ERROR("gpio: invalid pin %d (valid: 1..%d)", pin, GPIO_PIN_MAX);
        return E_INVAL;
    }
    if (value != 0 && value != 1) {
        LOG_ERROR("gpio: invalid value %d (0 or 1)", value);
        return E_INVAL;
    }

    LOG_INFO("gpio mock: GPIO%d → %s", pin, value ? "HIGH" : "LOW");
    return E_OK;
}

void gpio_hal_close(void)
{
    g_gpio_initialized = 0;
    LOG_INFO("gpio hal closed (mock mode)");
}

#else /* GPIO_REAL_HW — libgpiod */

/* ─── 真实硬件模式 ─────────────────────────────────────── */

int gpio_hal_init(void)
{
    /* 打开 /dev/gpiochip0 */
    g_chip = gpiod_chip_open("/dev/gpiochip0");
    if (!g_chip) {
        LOG_WARN("gpio: gpiod_chip_open /dev/gpiochip0 failed, "
                 "falling back to mock mode");
        g_gpio_initialized = 1;
        return E_OK;
    }

    LOG_INFO("gpio hal init: real hw (libgpiod) on /dev/gpiochip0");
    g_gpio_initialized = 1;
    return E_OK;
}

int gpio_hal_set(int pin, int value)
{
    if (!g_gpio_initialized) return E_INVAL;
    if (pin < 1 || pin > GPIO_PIN_MAX) {
        LOG_ERROR("gpio: invalid pin %d (valid: 1..%d)", pin, GPIO_PIN_MAX);
        return E_INVAL;
    }

    /* 如果 libgpiod 不可用，降级 mock */
    if (!g_chip) {
        LOG_INFO("gpio mock: GPIO%d → %s", pin, value ? "HIGH" : "LOW");
        return E_OK;
    }

    /* 按需获取 line（懒加载，仅首次设置时申请） */
    if (!g_lines[pin]) {
        /* 实际引脚号由具体硬件决定，这里简化处理 */
        int offset = pin - 1;
        g_lines[pin] = gpiod_chip_get_line(g_chip, offset);
        if (!g_lines[pin]) {
            LOG_ERROR("gpio: get_line pin=%d offset=%d failed", pin, offset);
            return E_IO;
        }
        if (gpiod_line_request_output(g_lines[pin], "embmqttnode", 0) < 0) {
            LOG_ERROR("gpio: request_output pin=%d failed", pin);
            gpiod_line_release(g_lines[pin]);
            g_lines[pin] = NULL;
            return E_IO;
        }
        LOG_INFO("gpio: pin %d claimed as output", pin);
    }

    if (gpiod_line_set_value(g_lines[pin], value) < 0) {
        LOG_ERROR("gpio: set_value pin=%d to %d failed", pin, value);
        return E_IO;
    }

    LOG_INFO("gpio: GPIO%d → %s", pin, value ? "HIGH" : "LOW");
    return E_OK;
}

void gpio_hal_close(void)
{
    if (g_chip) {
        for (int i = 1; i <= GPIO_PIN_MAX; i++) {
            if (g_lines[i]) {
                gpiod_line_set_value(g_lines[i], 0);
                gpiod_line_release(g_lines[i]);
                g_lines[i] = NULL;
            }
        }
        gpiod_chip_close(g_chip);
        g_chip = NULL;
    }
    g_gpio_initialized = 0;
    LOG_INFO("gpio hal closed (real hw)");
}

#endif /* GPIO_REAL_HW */
