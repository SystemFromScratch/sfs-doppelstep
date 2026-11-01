/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * DYNAMIXEL Protocol 2.0 bus with one emulated XM430-W350 (see dxl_bus.h).
 *
 * Threads and contexts (X430 integration guide, "RTOS" layout):
 *   UART ISR        bytes -> ring buffer, wakes the bus thread
 *   bus thread      x430_bus_receive() (parser, answers, hooks; bus lock held
 *                   inside), x430_bus_poll() (Bus Watchdog) and the refresh of
 *                   the live registers every CONFIG_SFS_DXL_POLL_MS
 *   hooks           run in the bus thread; motion commands go to the slow-loop
 *                   thread through motor_*() (synchronous, < 2 slow-loop periods)
 */

#include "dxl_bus.h"

#include <errno.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <x430.h>

#include "joint.h"
#include "motor.h"
#include "nvm.h"

LOG_MODULE_REGISTER(dxl_bus, CONFIG_SFS_LOG_LEVEL);

#define DXL_UART_NODE DT_CHOSEN(sfs_dxl_uart)

#if !DT_NODE_EXISTS(DXL_UART_NODE)
#error "Set /chosen/sfs,dxl-uart to the UART of the DYNAMIXEL bus"
#endif

static const struct device *const uart = DEVICE_DT_GET(DXL_UART_NODE);

/* Baud Rate (8) codes of the X430 Control Table (ROBOTIS e-Manual) */
static const uint32_t baud_bps[] = {9600U,    57600U,   115200U,  1000000U,
				    2000000U, 3000000U, 4000000U, 4500000U};

RING_BUF_DECLARE(rx_ring, CONFIG_SFS_DXL_RX_BUFFER_SIZE);
static K_SEM_DEFINE(rx_sem, 0, 1);
static atomic_t rx_overruns;

K_MUTEX_DEFINE(bus_mutex); /* Zephyr mutexes are recursive */

static struct x430_bus bus;
static struct x430_device joint_dev;

static const struct joint_cfg jcfg = {
	.full_steps_per_rev = CONFIG_SFS_MOTOR_FULL_STEPS_PER_REV,
	.microsteps = CONFIG_SFS_CTRL_MICROSTEPS,
	.gear_motor_revs = CONFIG_SFS_DXL_GEAR_MOTOR_REVS,
	.gear_joint_revs = CONFIG_SFS_DXL_GEAR_JOINT_REVS,
	.start_position = CONFIG_SFS_DXL_START_POSITION,
};

/* Control Table values the mapping needs, kept current by the write hook. */
static struct {
	struct joint_regs regs;
	uint8_t operating_mode;
	uint8_t baud;
	uint32_t profile_velocity;
	uint32_t profile_acceleration;
	uint32_t velocity_limit;
} cache;

static bool eeprom_missing;       /* no EEPROM block at startup */
static bool factory_baud_pending; /* apply CONFIG_SFS_DXL_FACTORY_BAUD_CODE */
static uint16_t faults_reported;

static K_THREAD_STACK_DEFINE(dxl_stack, CONFIG_SFS_DXL_THREAD_STACK_SIZE);
static struct k_thread dxl_thread;

/* ------------------------------------------------------------------------ */
/* UART                                                                     */

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	uart_irq_update(dev);
	while (uart_irq_rx_ready(dev) != 0) {
		uint8_t buf[16];
		const int n = uart_fifo_read(dev, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}
		if (ring_buf_put(&rx_ring, buf, (uint32_t)n) != (uint32_t)n) {
			(void)atomic_inc(&rx_overruns);
		}
		k_sem_give(&rx_sem);
	}
}

static int uart_write(void *context, const uint8_t *data, size_t length)
{
	ARG_UNUSED(context);
	for (size_t i = 0; i < length; i++) {
		uart_poll_out(uart, data[i]);
	}
	return 0;
}

static int uart_set_baud(uint8_t code)
{
	struct uart_config cfg;
	int ret;

	if (code >= ARRAY_SIZE(baud_bps)) {
		return -EINVAL;
	}
	ret = uart_config_get(uart, &cfg);
	if ((ret == 0) && (cfg.baudrate != baud_bps[code])) {
		cfg.baudrate = baud_bps[code];
		ret = uart_configure(uart, &cfg);
	}
	if (ret != 0) {
		LOG_ERR("UART %u bps: %d", baud_bps[code], ret);
	} else {
		LOG_INF("bus at %u bps", baud_bps[code]);
	}
	return ret;
}

/* ------------------------------------------------------------------------ */
/* X430 port                                                                */

static void port_lock(void *context)
{
	(void)k_mutex_lock(context, K_FOREVER);
}

static void port_unlock(void *context)
{
	(void)k_mutex_unlock(context);
}

static uint32_t port_millis(void *context)
{
	ARG_UNUSED(context);
	return k_uptime_get_32();
}

static void port_delay_us(void *context, uint32_t us)
{
	ARG_UNUSED(context);
	k_busy_wait(us);
}

static const struct x430_port port = {
	.context = &bus_mutex,
	.lock = port_lock,
	.unlock = port_unlock,
	.millis = port_millis,
	.delay_us = port_delay_us,
};

/* ------------------------------------------------------------------------ */
/* Control Table <-> motor                                                  */

static int32_t reg(uint16_t address)
{
	return x430_device_get(&joint_dev, address);
}

static void reload_cache(void)
{
	cache.regs.drive_mode = (uint8_t)reg(X430_ADDR_DRIVE_MODE);
	cache.regs.homing_offset = reg(X430_ADDR_HOMING_OFFSET);
	cache.operating_mode = (uint8_t)reg(X430_ADDR_OPERATING_MODE);
	cache.baud = (uint8_t)reg(X430_ADDR_BAUD_RATE);
	cache.profile_velocity = (uint32_t)reg(X430_ADDR_PROFILE_VELOCITY);
	cache.profile_acceleration = (uint32_t)reg(X430_ADDR_PROFILE_ACCELERATION);
	cache.velocity_limit = (uint32_t)reg(X430_ADDR_VELOCITY_LIMIT);
}

static uint8_t hardware_error_bits(uint16_t faults)
{
	uint8_t bits = 0U;

	if ((faults & (SSTEP_FAULT_OVERVOLTAGE | SSTEP_FAULT_UNDERVOLTAGE)) != 0U) {
		bits |= X430_HW_ERROR_INPUT_VOLTAGE;
	}
	if ((faults & (SSTEP_FAULT_OVERCURRENT | SSTEP_FAULT_EXTERNAL | SSTEP_FAULT_HAL)) != 0U) {
		bits |= X430_HW_ERROR_ELECTRICAL_SHOCK;
	}
	return bits;
}

/* Live registers from the motor snapshot. Bus lock held. */
static void refresh_live(void)
{
	struct motor_snapshot snap;
	const struct sstep_status *st = &snap.status;

	motor_get_snapshot(&snap);

	const int32_t position = joint_position_to_dxl(&jcfg, &cache.regs, st->position);
	const int32_t velocity = joint_velocity_to_dxl(&jcfg, &cache.regs, st->velocity);
	const bool moving = st->state == SSTEP_STATE_MOVING;
	uint8_t moving_status = 0U;

	(void)x430_device_set(&joint_dev, X430_ADDR_PRESENT_POSITION, position);
	(void)x430_device_set(&joint_dev, X430_ADDR_POSITION_TRAJECTORY, position);
	(void)x430_device_set(&joint_dev, X430_ADDR_PRESENT_VELOCITY, velocity);
	(void)x430_device_set(&joint_dev, X430_ADDR_VELOCITY_TRAJECTORY, velocity);
	(void)x430_device_set(&joint_dev, X430_ADDR_PRESENT_CURRENT,
			      joint_current_to_dxl((int32_t)snap.i_amplitude_ma));
	(void)x430_device_set(&joint_dev, X430_ADDR_PRESENT_INPUT_VOLTAGE,
			      (int32_t)((st->vbus_mv + 50U) / 100U));
	(void)x430_device_set(&joint_dev, X430_ADDR_MOVING, moving ? 1 : 0);

	if (moving) {
		moving_status |= X430_MOVING_STATUS_PROFILE_ONGOING |
				 X430_MOVING_STATUS_PROFILE_TRAPEZOIDAL;
	} else if (st->position == st->target) {
		moving_status |= X430_MOVING_STATUS_IN_POSITION;
	}
	(void)x430_device_set(&joint_dev, X430_ADDR_MOVING_STATUS, moving_status);

	/* Faults of the power stage -> Hardware Error Status (once per new fault) */
	const uint16_t fresh = st->faults & (uint16_t)~faults_reported;

	if (fresh != 0U) {
		const uint8_t bits = hardware_error_bits(fresh);

		if (bits != 0U) {
			x430_device_raise_hardware_error(&joint_dev, bits);
		}
	}
	faults_reported = st->faults;

	/* Outputs off while Torque Enable says on (fault, external disable) */
	if ((reg(X430_ADDR_TORQUE_ENABLE) != 0) &&
	    ((st->state == SSTEP_STATE_DISABLED) || (st->state == SSTEP_STATE_FAULT))) {
		(void)x430_device_set(&joint_dev, X430_ADDR_TORQUE_ENABLE, 0);
	}
}

static uint8_t to_status(int rc)
{
	return (rc == 0) ? DXL2_ERR_NONE : DXL2_ERR_RESULT_FAIL;
}

static uint8_t goal_position(int32_t value)
{
	struct motor_snapshot snap;
	struct joint_profile profile;
	int32_t target;

	if (((cache.operating_mode != X430_MODE_POSITION) &&
	     (cache.operating_mode != X430_MODE_EXTENDED_POSITION)) ||
	    (reg(X430_ADDR_TORQUE_ENABLE) == 0)) {
		return DXL2_ERR_NONE; /* stored only, as on an X430 */
	}

	motor_get_snapshot(&snap);
	target = joint_position_from_dxl(&jcfg, &cache.regs, value);
	profile = joint_profile(&jcfg, &cache.regs,
				(uint32_t)llabs((int64_t)target - snap.status.position),
				cache.profile_velocity, cache.profile_acceleration,
				cache.velocity_limit, CONFIG_SFS_CTRL_V_MAX,
				CONFIG_SFS_CTRL_ACCEL_MAX);

	return to_status(motor_move_to(target, profile.v_max, profile.accel));
}

static uint8_t goal_velocity(int32_t value)
{
	uint32_t cap = CONFIG_SFS_CTRL_V_MAX;
	uint32_t accel = CONFIG_SFS_CTRL_ACCEL_MAX;
	int32_t velocity;

	if ((cache.operating_mode != X430_MODE_VELOCITY) || (reg(X430_ADDR_TORQUE_ENABLE) == 0)) {
		return DXL2_ERR_NONE;
	}

	if (cache.velocity_limit > 0U) {
		cap = MIN(cap, joint_speed_from_dxl(&jcfg, cache.velocity_limit));
	}
	if (cache.profile_acceleration > 0U) {
		accel = CLAMP(joint_accel_from_dxl(&jcfg, cache.profile_acceleration), 1U,
			      (uint32_t)CONFIG_SFS_CTRL_ACCEL_MAX);
	}
	velocity = CLAMP(joint_velocity_from_dxl(&jcfg, &cache.regs, value), -(int32_t)cap,
			 (int32_t)cap);

	return to_status(motor_run(velocity, MAX((uint32_t)abs(velocity), 1U), accel));
}

/* ------------------------------------------------------------------------ */
/* X430 hooks (bus thread, bus lock held)                                   */

static uint8_t hook_write(struct x430_device *device, uint16_t address, uint8_t size,
			  int32_t value, void *user_data)
{
	ARG_UNUSED(device);
	ARG_UNUSED(size);
	ARG_UNUSED(user_data);

	switch (address) {
	case X430_ADDR_TORQUE_ENABLE:
		if (value != 0) {
			/* The library copies Present Position into Goal Position next */
			refresh_live();
			return to_status(motor_enable(true));
		}
		return to_status(motor_enable(false));

	case X430_ADDR_OPERATING_MODE:
		/* Position, extended position and velocity map onto Stepper-C;
		 * current, current-based position and PWM need a torque path.
		 */
		if ((value != X430_MODE_POSITION) && (value != X430_MODE_EXTENDED_POSITION) &&
		    (value != X430_MODE_VELOCITY)) {
			return DXL2_ERR_DATA_RANGE;
		}
		cache.operating_mode = (uint8_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_DRIVE_MODE:
		cache.regs.drive_mode = (uint8_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_HOMING_OFFSET:
		cache.regs.homing_offset = value;
		return DXL2_ERR_NONE;

	case X430_ADDR_BAUD_RATE:
		if ((uint32_t)value >= ARRAY_SIZE(baud_bps)) {
			return DXL2_ERR_DATA_RANGE;
		}
		cache.baud = (uint8_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_PROFILE_VELOCITY:
		cache.profile_velocity = (uint32_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_PROFILE_ACCELERATION:
		cache.profile_acceleration = (uint32_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_VELOCITY_LIMIT:
		cache.velocity_limit = (uint32_t)value;
		return DXL2_ERR_NONE;

	case X430_ADDR_CURRENT_LIMIT:
		return to_status(motor_set_current(joint_current_from_dxl((uint32_t)value),
						   CONFIG_SFS_CTRL_I_HOLD_MA));

	case X430_ADDR_GOAL_POSITION:
		return goal_position(value);

	case X430_ADDR_GOAL_VELOCITY:
		return goal_velocity(value);

	default:
		return DXL2_ERR_NONE;
	}
}

static void hook_read(struct x430_device *device, uint16_t address, uint16_t length,
		      void *user_data)
{
	ARG_UNUSED(device);
	ARG_UNUSED(user_data);

	if ((address + length) > X430_ADDR_REALTIME_TICK) {
		refresh_live();
	}
}

static uint8_t hook_event(struct x430_device *device, enum x430_event event, void *user_data)
{
	ARG_UNUSED(device);
	ARG_UNUSED(user_data);

	switch (event) {
	case X430_EVENT_REBOOT:
		(void)motor_enable(false);
		(void)motor_clear_faults();
		faults_reported = 0U;
		reload_cache();
		break;

	case X430_EVENT_FACTORY_RESET: {
		const uint8_t previous_baud = cache.baud;

		(void)motor_enable(false);
		(void)motor_clear_faults();
		faults_reported = 0U;
		reload_cache();
		/* Baud Rate went back to the X430 factory value: use this
		 * firmware's factory value instead (bus thread, after the answer).
		 */
		if ((cache.baud != previous_baud) &&
		    (cache.baud != CONFIG_SFS_DXL_FACTORY_BAUD_CODE)) {
			factory_baud_pending = true;
		}
		break;
	}

	case X430_EVENT_CLEAR_MULTI_TURN: {
		struct motor_snapshot snap;

		motor_get_snapshot(&snap);
		const int32_t present = joint_position_to_dxl(&jcfg, &cache.regs,
							      snap.status.position);
		const int32_t wrapped = ((present % JOINT_DXL_PULSES_PER_REV) +
					 JOINT_DXL_PULSES_PER_REV) %
					JOINT_DXL_PULSES_PER_REV;

		return to_status(
			motor_set_position(joint_position_from_dxl(&jcfg, &cache.regs, wrapped)));
	}

	case X430_EVENT_BUS_WATCHDOG:
		(void)motor_stop();
		break;

	case X430_EVENT_BAUD_RATE:
		cache.baud = (uint8_t)reg(X430_ADDR_BAUD_RATE);
		(void)uart_set_baud(cache.baud);
		break;

	case X430_EVENT_TORQUE_SHUTDOWN:
		(void)motor_enable(false);
		break;

	case X430_EVENT_BACKUP_RESTORED:
		reload_cache();
		(void)motor_set_current(
			joint_current_from_dxl((uint32_t)reg(X430_ADDR_CURRENT_LIMIT)),
			CONFIG_SFS_CTRL_I_HOLD_MA);
		break;

	default:
		break;
	}
	return DXL2_ERR_NONE;
}

static int hook_nvm_load(struct x430_device *device, enum x430_nvm_slot slot, uint8_t *data,
			 uint16_t size, void *user_data)
{
	const int ret = nvm_load((uint8_t)slot, data, size);

	ARG_UNUSED(device);
	ARG_UNUSED(user_data);
	if ((ret != 0) && (slot == X430_NVM_EEPROM)) {
		eeprom_missing = true;
	}
	return ret;
}

static int hook_nvm_store(struct x430_device *device, enum x430_nvm_slot slot,
			  const uint8_t *data, uint16_t size, void *user_data)
{
	struct motor_snapshot snap;

	ARG_UNUSED(device);
	ARG_UNUSED(user_data);

	/* A flash erase stalls the CPU (single bank) and with it the current loop */
	motor_get_snapshot(&snap);
	if (snap.status.state != SSTEP_STATE_DISABLED) {
		LOG_WRN("NVM write refused: power stage enabled");
		return -EBUSY;
	}
	return nvm_store((uint8_t)slot, data, size);
}

static const struct x430_hooks hooks = {
	.write = hook_write,
	.read = hook_read,
	.event = hook_event,
	.nvm_load = hook_nvm_load,
	.nvm_store = hook_nvm_store,
};

/* Write this firmware's factory Baud Rate through the normal write path
 * (validated, stored in the EEPROM block, X430_EVENT_BAUD_RATE). Bus lock held.
 */
static void apply_factory_baud(void)
{
	const uint8_t code = CONFIG_SFS_DXL_FACTORY_BAUD_CODE;
	const uint8_t rc = x430_device_write(&joint_dev, X430_ADDR_BAUD_RATE, 1U, &code, 0U);

	if (rc != DXL2_ERR_NONE) {
		LOG_ERR("factory Baud Rate %u: error 0x%02x", code, rc);
	}
	x430_device_flush_events(&joint_dev);
}

/* ------------------------------------------------------------------------ */

static void dxl_thread_fn(void *p1, void *p2, void *p3)
{
	int64_t next_poll = k_uptime_get() + CONFIG_SFS_DXL_POLL_MS;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		uint8_t chunk[64];
		uint32_t n;

		(void)k_sem_take(&rx_sem, K_MSEC(CONFIG_SFS_DXL_POLL_MS));

		while ((n = ring_buf_get(&rx_ring, chunk, sizeof(chunk))) > 0U) {
			x430_bus_receive(&bus, chunk, n);
		}

		if (factory_baud_pending) {
			x430_bus_lock(&bus);
			factory_baud_pending = false;
			apply_factory_baud();
			x430_bus_unlock(&bus);
		}

		if (k_uptime_get() >= next_poll) {
			next_poll += CONFIG_SFS_DXL_POLL_MS;
			x430_bus_poll(&bus);
			x430_bus_lock(&bus);
			refresh_live();
			x430_bus_unlock(&bus);

			const atomic_val_t overruns = atomic_set(&rx_overruns, 0);

			if (overruns != 0) {
				LOG_WRN("RX ring buffer overrun (%ld)", (long)overruns);
			}
		}
	}
}

int dxl_bus_init(void)
{
	const struct x430_bus_config bus_cfg = {
		.write = uart_write,
		.write_context = NULL,
		.port = &port,
	};
	const struct x430_device_config dev_cfg = {
		.id = CONFIG_SFS_DXL_ID,
		.hooks = &hooks,
		.user_data = NULL,
	};
	int ret;

	if (!device_is_ready(uart)) {
		LOG_ERR("%s not ready", uart->name);
		return -ENODEV;
	}

	ret = nvm_init();
	if (ret != 0) {
		LOG_WRN("NVM unavailable (%d): EEPROM area not persistent", ret);
	}

	if ((x430_bus_init(&bus, &bus_cfg) != X430_OK) ||
	    (x430_device_init(&joint_dev, &dev_cfg) != X430_OK) ||
	    (x430_bus_attach(&bus, &joint_dev) != X430_OK)) {
		LOG_ERR("X430 initialisation failed");
		return -EINVAL;
	}

	x430_bus_lock(&bus);
	reload_cache();
	if (eeprom_missing && (cache.baud != CONFIG_SFS_DXL_FACTORY_BAUD_CODE)) {
		apply_factory_baud(); /* first start: this firmware's factory baud rate */
	}
	(void)uart_set_baud(cache.baud);
	refresh_live();
	x430_bus_unlock(&bus);

	uart_irq_callback_set(uart, uart_isr);
	uart_irq_rx_enable(uart);

	(void)k_thread_create(&dxl_thread, dxl_stack, K_THREAD_STACK_SIZEOF(dxl_stack),
			      dxl_thread_fn, NULL, NULL, NULL, CONFIG_SFS_DXL_THREAD_PRIORITY, 0,
			      K_NO_WAIT);
	(void)k_thread_name_set(&dxl_thread, "dxl");

	LOG_INF("DYNAMIXEL %s: ID %u, model %u, %s", CONFIG_X430_MODEL_NAME,
		x430_device_id(&joint_dev), (unsigned int)reg(X430_ADDR_MODEL_NUMBER),
		uart->name);
	return 0;
}
