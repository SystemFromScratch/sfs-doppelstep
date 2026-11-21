/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bit stream of the STM32 WS2812 driver (ws2812_encode.h): one compare value per bit,
 * MSB first, in the colour order of the chain. The timer and DMA part needs the board.
 */

#include <zephyr/ztest.h>

#include "ws2812_encode.h"

#define T0 68  /* 0.4 us at 170 MHz */
#define T1 136 /* 0.8 us */

static const uint8_t grb[] = {LED_COLOR_ID_GREEN, LED_COLOR_ID_RED, LED_COLOR_ID_BLUE};

ZTEST(ws2812, test_one_pixel_is_24_values_in_grb_order_msb_first)
{
	const struct led_rgb px = {.r = 0x80, .g = 0x01, .b = 0xFF};
	uint8_t out[24];

	zassert_equal(ws2812_encode(&px, 1, grb, 3, T0, T1, out), 24);
	/* green 0x01: seven zeros, then a one */
	for (int i = 0; i < 7; i++) {
		zassert_equal(out[i], T0);
	}
	zassert_equal(out[7], T1);
	/* red 0x80: a one, then seven zeros */
	zassert_equal(out[8], T1);
	for (int i = 9; i < 16; i++) {
		zassert_equal(out[i], T0);
	}
	/* blue 0xFF: all ones */
	for (int i = 16; i < 24; i++) {
		zassert_equal(out[i], T1);
	}
}

ZTEST(ws2812, test_dark_pixels_are_all_zero_bits)
{
	const struct led_rgb px[2] = {0};
	uint8_t out[48];

	zassert_equal(ws2812_encode(px, 2, grb, 3, T0, T1, out), 48);
	for (int i = 0; i < 48; i++) {
		zassert_equal(out[i], T0);
	}
}

ZTEST(ws2812, test_white_channel_is_sent_dark_and_unknown_colours_fail)
{
	static const uint8_t grbw[] = {LED_COLOR_ID_GREEN, LED_COLOR_ID_RED, LED_COLOR_ID_BLUE,
				       LED_COLOR_ID_WHITE};
	static const uint8_t bad[] = {LED_COLOR_ID_GREEN, 0x7F, LED_COLOR_ID_BLUE};
	const struct led_rgb px = {.r = 0xFF, .g = 0xFF, .b = 0xFF};
	uint8_t out[32];

	zassert_equal(ws2812_encode(&px, 1, grbw, 4, T0, T1, out), 32);
	for (int i = 24; i < 32; i++) {
		zassert_equal(out[i], T0, "no white in the led_rgb API");
	}
	zassert_equal(ws2812_encode(&px, 1, bad, 3, T0, T1, out), -1);
}

ZTEST_SUITE(ws2812, NULL, NULL, NULL, NULL, NULL);
