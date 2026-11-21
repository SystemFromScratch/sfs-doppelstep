/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * STATUS LED of the board: a GPIO (alias led0, B-G431B-ESC1) or the first pixel of an
 * LED strip (chosen sfs,status-led-strip, the WS2812B chain of the full-bridge board).
 * Without either, the calls do nothing.
 */

#ifndef DOPPELSTEP_APP_STATUS_LED_H_
#define DOPPELSTEP_APP_STATUS_LED_H_

#include <stdbool.h>

/** Configure the LED and switch it off. Main thread, once. */
void status_led_init(void);

/** Switch the LED; a strip is written only when the state changes. Main thread. */
void status_led_set(bool on);

#endif /* DOPPELSTEP_APP_STATUS_LED_H_ */
