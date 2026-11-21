/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * WS2812 LED chain on an STM32 timer channel with DMA (compatible "sfs,ws2812-stm32-tim"),
 * a driver of the Zephyr led_strip class.
 *
 * The timer runs in edge-aligned PWM mode 1 with a period of one WS2812 bit (1.25 us)
 * and its compare register preloaded. On every update event the DMA writes the compare
 * value of the next bit (t0h or t1h), which becomes active at the following update. The
 * stream ends with two zeros, which keep the pin low: the chain latches after
 * reset-delay. The CPU only starts the transfer, so the zero-latency fast loop of the
 * power stage cannot stretch a bit, and the transfer cannot delay the fast loop.
 *
 * Why not Zephyr's worldsemi,ws2812-uart: it needs the asynchronous UART API, the DMA
 * driver and the system work queue, about 10 KB of flash, and the application slot of
 * the full-bridge board has not got them (docs/hardware-full-bridge.md). This driver
 * programs the timer, one DMA channel and its DMAMUX channel itself, so their nodes stay
 * disabled.
 */

#define DT_DRV_COMPAT sfs_ws2812_stm32_tim

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/stm32_clock_control.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stm32_ll_dma.h>
#include <stm32_ll_tim.h>

#include "ws2812_encode.h"

LOG_MODULE_REGISTER(ws2812_stm32_tim, CONFIG_LED_STRIP_LOG_LEVEL);

BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1, "one chain");

#define WS_NODE     DT_DRV_INST(0)
#define TIM_NODE    DT_PHANDLE(WS_NODE, timer)
#define DMAMUX_NODE DT_DMAS_CTLR_BY_IDX(WS_NODE, 0)
#define DMA_NODE    DT_NODELABEL(dma1)

/* LL programs these peripherals directly: no Zephyr driver may own them. */
BUILD_ASSERT(!DT_NODE_HAS_STATUS(TIM_NODE, okay), "WS2812 timer must be disabled");
BUILD_ASSERT(!DT_NODE_HAS_STATUS(DMAMUX_NODE, okay) && !DT_NODE_HAS_STATUS(DMA_NODE, okay),
	     "dma1 and dmamux1 must be disabled: this driver programs them");

#define TIMx       ((TIM_TypeDef *)DT_REG_ADDR(TIM_NODE))
#define DMAx       ((DMA_TypeDef *)DT_REG_ADDR(DMA_NODE))
#define DMA_CH     DT_DMAS_CELL_BY_IDX(WS_NODE, 0, channel) /* LL_DMA_CHANNEL_1 = 0 */
#define DMA_REQ    DT_DMAS_CELL_BY_IDX(WS_NODE, 0, slot)
#define TIM_CH_NUM DT_PROP(WS_NODE, channel)

#define WS_PIXELS DT_PROP(WS_NODE, chain_length)
#define WS_COLORS DT_PROP_LEN(WS_NODE, color_mapping)
#define WS_BITS   (WS_PIXELS * WS_COLORS * 8U)
/* two zero periods after the data: the last bit ends low and the pin stays low */
#define TRAILER   2U

PINCTRL_DT_DEFINE(WS_NODE);

static const uint8_t color_mapping[] = DT_PROP(WS_NODE, color_mapping);
static const struct stm32_pclken tim_clk[] = STM32_DT_CLOCKS(TIM_NODE);
static const struct stm32_pclken dma_clk[] = STM32_DT_CLOCKS(DMA_NODE);
static const struct stm32_pclken dmamux_clk[] = STM32_DT_CLOCKS(DMAMUX_NODE);

static uint8_t stream[WS_BITS + TRAILER];
static uint8_t t0;
static uint8_t t1;
static uint32_t period_ns;
static K_MUTEX_DEFINE(lock);

static volatile uint32_t *ccr(void)
{
	switch (TIM_CH_NUM) {
	case 1:
		return &TIMx->CCR1;
	case 2:
		return &TIMx->CCR2;
	case 3:
		return &TIMx->CCR3;
	default:
		return &TIMx->CCR4;
	}
}

static uint32_t ll_channel(void)
{
	static const uint32_t ch[] = {LL_TIM_CHANNEL_CH1, LL_TIM_CHANNEL_CH2, LL_TIM_CHANNEL_CH3,
				      LL_TIM_CHANNEL_CH4};

	return ch[TIM_CH_NUM - 1];
}

/* Transfer complete flag of the channel: 4 flag bits per channel in DMA_ISR */
static bool transfer_done(void)
{
	return (DMAx->ISR & (DMA_ISR_TCIF1 << (4U * DMA_CH))) != 0U;
}

static int ws2812_update_rgb(const struct device *dev, struct led_rgb *pixels, size_t num_pixels)
{
	int n;
	int ret = 0;

	ARG_UNUSED(dev);

	k_mutex_lock(&lock, K_FOREVER);
	n = ws2812_encode(pixels, num_pixels, color_mapping, WS_COLORS, t0, t1, stream);
	if (n < 0) {
		k_mutex_unlock(&lock);
		return -EINVAL;
	}
	stream[n] = 0U;
	stream[n + 1] = 0U;

	LL_DMA_DisableChannel(DMAx, DMA_CH);
	DMAx->IFCR = (DMA_IFCR_CGIF1 << (4U * DMA_CH));
	LL_DMA_SetMemoryAddress(DMAx, DMA_CH, (uint32_t)stream);
	LL_DMA_SetDataLength(DMAx, DMA_CH, (uint32_t)n + TRAILER);
	LL_DMA_EnableChannel(DMAx, DMA_CH);

	/* From the next update on, one compare value per period */
	LL_TIM_ClearFlag_UPDATE(TIMx);
	LL_TIM_EnableDMAReq_UPDATE(TIMx);

	/* About 30 us per pixel; no interrupt: the caller is a thread and sleeps meanwhile */
	const uint32_t us = (((uint32_t)n + TRAILER + 1U) * period_ns + 999U) / 1000U;

	k_usleep(us);
	for (int i = 0; (i < 100) && !transfer_done(); i++) {
		k_busy_wait(10);
	}
	if (!transfer_done()) {
		ret = -EIO;
	}
	LL_TIM_DisableDMAReq_UPDATE(TIMx);
	LL_DMA_DisableChannel(DMAx, DMA_CH);
	*ccr() = 0U; /* pin low until the next update */

	/* Latch: the chain takes the new colours after reset-delay of low level */
	k_usleep(DT_PROP(WS_NODE, reset_delay));
	k_mutex_unlock(&lock);
	return ret;
}

static size_t ws2812_length(const struct device *dev)
{
	ARG_UNUSED(dev);
	return WS_PIXELS;
}

static DEVICE_API(led_strip, ws2812_api) = {
	.update_rgb = ws2812_update_rgb,
	.length = ws2812_length,
};

static int ws2812_init(const struct device *dev)
{
	const struct device *const clk = DEVICE_DT_GET(STM32_CLOCK_CONTROL_NODE);
	uint32_t rate = 0U;
	int ret;

	ARG_UNUSED(dev);

	if (!device_is_ready(clk)) {
		return -ENODEV;
	}
	ret = clock_control_on(clk, (clock_control_subsys_t)&tim_clk[0]);
	if (ret == 0) {
		ret = clock_control_on(clk, (clock_control_subsys_t)&dma_clk[0]);
	}
	if (ret == 0) {
		ret = clock_control_on(clk, (clock_control_subsys_t)&dmamux_clk[0]);
	}
	if (ret == 0) {
		ret = clock_control_get_rate(clk, (clock_control_subsys_t)&tim_clk[1], &rate);
	}
	if (ret != 0) {
		LOG_ERR("clock setup failed (%d)", ret);
		return ret;
	}

	/* Ticks of one bit and of the two high times, rounded */
	const uint32_t ns = DT_PROP(WS_NODE, period_ns);
	const uint32_t period = (uint32_t)(((uint64_t)rate * ns + 500000000U) / 1000000000U);
	const uint32_t h0 =
		(uint32_t)(((uint64_t)rate * DT_PROP(WS_NODE, t0h_ns) + 500000000U) / 1000000000U);
	const uint32_t h1 =
		(uint32_t)(((uint64_t)rate * DT_PROP(WS_NODE, t1h_ns) + 500000000U) / 1000000000U);

	if ((period < 2U) || (h1 >= period) || (h1 > UINT8_MAX) || (h0 == 0U)) {
		LOG_ERR("timer clock %u Hz does not fit the WS2812 timing in 8 bits", rate);
		return -EINVAL;
	}
	t0 = (uint8_t)h0;
	t1 = (uint8_t)h1;
	period_ns = ns;

	/* Timer: edge-aligned up-counter, one period per bit, compare preloaded, pin low */
	LL_TIM_DisableCounter(TIMx);
	LL_TIM_SetPrescaler(TIMx, 0U);
	LL_TIM_SetCounterMode(TIMx, LL_TIM_COUNTERMODE_UP);
	LL_TIM_SetAutoReload(TIMx, period - 1U);
	LL_TIM_EnableARRPreload(TIMx);
	LL_TIM_OC_SetMode(TIMx, ll_channel(), LL_TIM_OCMODE_PWM1);
	LL_TIM_OC_SetPolarity(TIMx, ll_channel(), LL_TIM_OCPOLARITY_HIGH);
	LL_TIM_OC_EnablePreload(TIMx, ll_channel());
	*ccr() = 0U;
	LL_TIM_CC_EnableChannel(TIMx, ll_channel());
	LL_TIM_GenerateEvent_UPDATE(TIMx);
	LL_TIM_EnableCounter(TIMx);

	/* DMA: memory bytes to the 32-bit compare register, request = timer update */
	LL_DMA_DisableChannel(DMAx, DMA_CH);
	LL_DMA_ConfigTransfer(DMAx, DMA_CH,
			      LL_DMA_DIRECTION_MEMORY_TO_PERIPH | LL_DMA_MODE_NORMAL |
				      LL_DMA_PERIPH_NOINCREMENT | LL_DMA_MEMORY_INCREMENT |
				      LL_DMA_PDATAALIGN_WORD | LL_DMA_MDATAALIGN_BYTE |
				      LL_DMA_PRIORITY_HIGH);
	LL_DMA_SetPeriphAddress(DMAx, DMA_CH, (uint32_t)ccr());
	LL_DMA_SetPeriphRequest(DMAx, DMA_CH, DMA_REQ);

	/* The pin follows the timer only now, already low */
	ret = pinctrl_apply_state(PINCTRL_DT_DEV_CONFIG_GET(WS_NODE), PINCTRL_STATE_DEFAULT);
	if (ret != 0) {
		LOG_ERR("pinctrl (%d)", ret);
		return ret;
	}
	LOG_DBG("%u pixels, period %u ticks, t0 %u, t1 %u", WS_PIXELS, period, t0, t1);
	return 0;
}

DEVICE_DT_INST_DEFINE(0, ws2812_init, NULL, NULL, NULL, POST_KERNEL, CONFIG_LED_STRIP_INIT_PRIORITY,
		      &ws2812_api);
