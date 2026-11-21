/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * WS2812 bit stream as compare values of a PWM timer, one per bit, MSB first, in the
 * colour order of the chain. Hardware-free, so that the host tests cover it.
 */

#ifndef DOPPELSTEP_WS2812_ENCODE_H_
#define DOPPELSTEP_WS2812_ENCODE_H_

#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/led_strip.h>
#include <zephyr/dt-bindings/led/led.h>

/**
 * @param pixels        Colours of the chain.
 * @param num_pixels    Pixels to send.
 * @param mapping       Colour order of one pixel (LED_COLOR_ID_*), @p num_colors entries.
 * @param num_colors    Colours per pixel (3 for GRB).
 * @param t0, t1        Compare values of a 0 and a 1 bit.
 * @param out           num_pixels * num_colors * 8 compare values.
 * @return Number of values written, or -1 for a colour the API has no value for.
 */
static inline int ws2812_encode(const struct led_rgb *pixels, size_t num_pixels,
				const uint8_t *mapping, size_t num_colors, uint8_t t0, uint8_t t1,
				uint8_t *out)
{
	size_t n = 0;

	for (size_t p = 0; p < num_pixels; p++) {
		for (size_t c = 0; c < num_colors; c++) {
			uint8_t v;

			switch (mapping[c]) {
			case LED_COLOR_ID_RED:
				v = pixels[p].r;
				break;
			case LED_COLOR_ID_GREEN:
				v = pixels[p].g;
				break;
			case LED_COLOR_ID_BLUE:
				v = pixels[p].b;
				break;
			case LED_COLOR_ID_WHITE:
				v = 0U;
				break;
			default:
				return -1;
			}
			for (int bit = 7; bit >= 0; bit--) {
				out[n++] = ((v >> bit) & 1U) ? t1 : t0;
			}
		}
	}
	return (int)n;
}

#endif /* DOPPELSTEP_WS2812_ENCODE_H_ */
