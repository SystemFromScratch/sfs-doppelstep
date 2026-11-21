/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * STATUS LED (status_led.h). On an LED strip the first pixel shows the state in red,
 * as the LED of an XM430; the other pixels of the chain stay dark. The brightness is
 * low on purpose: on the full-bridge board the strip and the MCU share a small 5 V
 * regulator (docs/hardware-full-bridge.md).
 */

#include "status_led.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(main, CONFIG_SFS_LOG_LEVEL);

#if DT_HAS_CHOSEN(sfs_status_led_strip)
#include <zephyr/drivers/led_strip.h>

#define STRIP_NODE   DT_CHOSEN(sfs_status_led_strip)
#define STRIP_PIXELS DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_PIXELS];
static bool strip_ready;
static bool strip_on;

static void strip_write(bool on)
{
	pixels[0].r = on ? CONFIG_SFS_STATUS_LED_BRIGHTNESS : 0U;
	if (led_strip_update_rgb(strip, pixels, ARRAY_SIZE(pixels)) == 0) {
		strip_on = on;
	}
}
#endif

/* GPIO LED on alias led0 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});
static bool led_ready;

void status_led_init(void)
{
	led_ready = (led.port != NULL) && gpio_is_ready_dt(&led) &&
		    (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) == 0);
#if DT_HAS_CHOSEN(sfs_status_led_strip)
	strip_ready = device_is_ready(strip);
	if (strip_ready) {
		/* Every pixel dark: some WS2812B light up at random after power-on */
		strip_write(false);
	} else {
		LOG_WRN("%s not ready: no STATUS LED", strip->name);
	}
#endif
}

void status_led_set(bool on)
{
	if (led_ready) {
		(void)gpio_pin_set_dt(&led, on ? 1 : 0);
	}
#if DT_HAS_CHOSEN(sfs_status_led_strip)
	if (strip_ready && (on != strip_on)) {
		strip_write(on);
	}
#endif
}
