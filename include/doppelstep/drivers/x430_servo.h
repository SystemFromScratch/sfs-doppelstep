/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Emulated DYNAMIXEL X430 servo on a UART bus (DYNAMIXEL Protocol 2.0), built on
 * the X430 Control Table emulator. The driver owns the UART interrupt, the receive
 * buffer, baud rate changes, the persistent EEPROM area and the factory baud rate.
 * The application owns the meaning of the registers.
 *
 * The driver has no thread: the application calls x430_servo_process() in a loop.
 * The callbacks run in that thread with the bus lock held. They may block for a
 * bounded time, because the status packet of the host waits for their result.
 */

#ifndef DOPPELSTEP_DRIVERS_X430_SERVO_H_
#define DOPPELSTEP_DRIVERS_X430_SERVO_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>

#include <x430.h>

#ifdef __cplusplus
extern "C" {
#endif

struct x430_servo_callbacks {
	/** A register was written; returns a DXL2_ERR_* for the status packet. */
	uint8_t (*write)(const struct device *dev, uint16_t address, uint8_t size, int32_t value,
			 void *user_data);
	/** Registers are about to be read: refresh the live ones. */
	void (*read)(const struct device *dev, uint16_t address, uint16_t length, void *user_data);
	/** Reboot, Factory Reset, Clear, Bus Watchdog...; returns a DXL2_ERR_*. */
	uint8_t (*event)(const struct device *dev, enum x430_event event, void *user_data);
	/** May the persistent area be written now? A flash erase can stall the CPU. */
	bool (*nvm_allowed)(const struct device *dev, void *user_data);
};

/** Install the callbacks and start receiving. Nothing reaches a callback before this. */
int x430_servo_start(const struct device *dev, const struct x430_servo_callbacks *callbacks,
		     void *user_data);

/**
 * Wait up to @p timeout for received bytes, parse them and answer, run the callbacks
 * and, every CONFIG_SFS_DXL_POLL_MS, the Bus Watchdog. With
 * CONFIG_SFS_DXL_REBOOT_RESETS_MCU it does not return after a Reboot instruction:
 * it resets the MCU once the status packet has left.
 */
int x430_servo_process(const struct device *dev, k_timeout_t timeout);

/** Take the bus lock. Callbacks already hold it. Recursive. */
void x430_servo_lock(const struct device *dev);

/** Release the bus lock. */
void x430_servo_unlock(const struct device *dev);

/** Value of a register (X430_ADDR_*). Bus lock held. */
int32_t x430_servo_reg_get(const struct device *dev, uint16_t address);

/** Set a live register (X430_ADDR_*) without the checks of a host write. Bus lock held. */
int x430_servo_reg_set(const struct device *dev, uint16_t address, int32_t value);

/** Raise bits of Hardware Error Status (X430_HW_ERROR_*). Bus lock held. */
void x430_servo_raise_hw_error(const struct device *dev, uint8_t bits);

#ifdef __cplusplus
}
#endif

#endif /* DOPPELSTEP_DRIVERS_X430_SERVO_H_ */
