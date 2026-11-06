/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * X430 Control Table emulator behind a Zephyr device, see x430_servo.h.
 *
 * Contexts (X430 integration guide, "RTOS" layout):
 *   UART ISR               bytes -> ring buffer, wakes the thread in process()
 *   x430_servo_process()   x430_bus_receive() (parser, answers, hooks; bus lock held
 *                          inside) and x430_bus_poll() (Bus Watchdog)
 */

#define DT_DRV_COMPAT sfs_x430_servo

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include <doppelstep/drivers/x430_servo.h>

#include "x430_nvm.h"

LOG_MODULE_REGISTER(x430_servo, CONFIG_X430_SERVO_LOG_LEVEL);

/* Baud Rate (8) codes of the X430 Control Table (ROBOTIS e-Manual) */
static const uint32_t baud_bps[] = {9600U,    57600U,   115200U,  1000000U,
				    2000000U, 3000000U, 4000000U, 4500000U};

struct x430_servo_config {
	const struct device *uart;
	const struct device *flash;
	off_t flash_offset;
	size_t flash_size;
	uint8_t factory_id;
	uint8_t factory_baud_code;
};

struct x430_servo_data {
	struct x430_bus bus;
	struct x430_device device;
	struct x430_port port;
	struct k_mutex mutex;
	struct x430_nvm nvm;

	uint8_t rx_storage[CONFIG_SFS_DXL_RX_BUFFER_SIZE];
	struct ring_buf rx_ring;
	struct k_sem rx_sem;
	atomic_t rx_overruns;

	const struct x430_servo_callbacks *cb;
	void *cb_data;

	uint8_t baud;
	bool eeprom_missing;       /* no EEPROM block at startup */
	bool factory_baud_pending; /* apply the factory-baud-code property */
	int64_t next_poll;
};

/* ------------------------------------------------------------------------ */
/* UART                                                                     */

static void uart_isr(const struct device *uart, void *user_data)
{
	struct x430_servo_data *data = user_data;

	uart_irq_update(uart);
	while (uart_irq_rx_ready(uart) != 0) {
		uint8_t buf[16];
		const int n = uart_fifo_read(uart, buf, sizeof(buf));

		if (n <= 0) {
			break;
		}
		if (ring_buf_put(&data->rx_ring, buf, (uint32_t)n) != (uint32_t)n) {
			(void)atomic_inc(&data->rx_overruns);
		}
		k_sem_give(&data->rx_sem);
	}
}

static int bus_write(void *context, const uint8_t *bytes, size_t length)
{
	const struct device *dev = context;
	const struct x430_servo_config *cfg = dev->config;

	for (size_t i = 0; i < length; i++) {
		uart_poll_out(cfg->uart, bytes[i]);
	}
	return 0;
}

static int uart_set_baud(const struct device *dev, uint8_t code)
{
	const struct x430_servo_config *cfg = dev->config;
	struct uart_config uart_cfg;
	int ret;

	if (code >= ARRAY_SIZE(baud_bps)) {
		return -EINVAL;
	}
	ret = uart_config_get(cfg->uart, &uart_cfg);
	if (ret == -ENOSYS) {
		/* no line rate (native_sim pseudoterminal) */
		LOG_INF("bus at %u bps (not settable)", baud_bps[code]);
		return 0;
	}
	if ((ret == 0) && (uart_cfg.baudrate != baud_bps[code])) {
		uart_cfg.baudrate = baud_bps[code];
		ret = uart_configure(cfg->uart, &uart_cfg);
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
	/* Zephyr mutexes are recursive */
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

/* ------------------------------------------------------------------------ */
/* X430 hooks (bus lock held)                                               */

static uint8_t hook_write(struct x430_device *device, uint16_t address, uint8_t size,
			  int32_t value, void *user_data)
{
	const struct device *dev = user_data;
	struct x430_servo_data *data = dev->data;

	ARG_UNUSED(device);

	if (address == X430_ADDR_BAUD_RATE) {
		if ((uint32_t)value >= ARRAY_SIZE(baud_bps)) {
			return DXL2_ERR_DATA_RANGE;
		}
		data->baud = (uint8_t)value;
	}
	if ((data->cb != NULL) && (data->cb->write != NULL)) {
		return data->cb->write(dev, address, size, value, data->cb_data);
	}
	return DXL2_ERR_NONE;
}

static void hook_read(struct x430_device *device, uint16_t address, uint16_t length,
		      void *user_data)
{
	const struct device *dev = user_data;
	const struct x430_servo_data *data = dev->data;

	ARG_UNUSED(device);

	if ((data->cb != NULL) && (data->cb->read != NULL)) {
		data->cb->read(dev, address, length, data->cb_data);
	}
}

static uint8_t hook_event(struct x430_device *device, enum x430_event event, void *user_data)
{
	const struct device *dev = user_data;
	const struct x430_servo_config *cfg = dev->config;
	struct x430_servo_data *data = dev->data;
	const uint8_t previous_baud = data->baud;
	uint8_t status = DXL2_ERR_NONE;

	ARG_UNUSED(device);

	if ((data->cb != NULL) && (data->cb->event != NULL)) {
		status = data->cb->event(dev, event, data->cb_data);
	}

	switch (event) {
	case X430_EVENT_BAUD_RATE:
		data->baud = (uint8_t)x430_device_get(&data->device, X430_ADDR_BAUD_RATE);
		(void)uart_set_baud(dev, data->baud);
		break;

	case X430_EVENT_FACTORY_RESET:
		data->baud = (uint8_t)x430_device_get(&data->device, X430_ADDR_BAUD_RATE);
		/* Baud Rate went back to the X430 factory value: use this firmware's
		 * factory value instead (in process(), after the answer).
		 */
		if ((data->baud != previous_baud) && (data->baud != cfg->factory_baud_code)) {
			data->factory_baud_pending = true;
		}
		break;

	case X430_EVENT_REBOOT:
	case X430_EVENT_BACKUP_RESTORED:
		data->baud = (uint8_t)x430_device_get(&data->device, X430_ADDR_BAUD_RATE);
		break;

	default:
		break;
	}
	return status;
}

static int hook_nvm_load(struct x430_device *device, enum x430_nvm_slot slot, uint8_t *bytes,
			 uint16_t size, void *user_data)
{
	const struct device *dev = user_data;
	struct x430_servo_data *data = dev->data;
	const int ret = x430_nvm_load(&data->nvm, (uint8_t)slot, bytes, size);

	ARG_UNUSED(device);

	if ((ret != 0) && (slot == X430_NVM_EEPROM)) {
		data->eeprom_missing = true;
	}
	return ret;
}

static int hook_nvm_store(struct x430_device *device, enum x430_nvm_slot slot,
			  const uint8_t *bytes, uint16_t size, void *user_data)
{
	const struct device *dev = user_data;
	struct x430_servo_data *data = dev->data;

	ARG_UNUSED(device);

	if ((data->cb != NULL) && (data->cb->nvm_allowed != NULL) &&
	    !data->cb->nvm_allowed(dev, data->cb_data)) {
		LOG_WRN("NVM write refused by the application");
		return -EBUSY;
	}
	return x430_nvm_store(&data->nvm, (uint8_t)slot, bytes, size);
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
static void apply_factory_baud(const struct device *dev)
{
	const struct x430_servo_config *cfg = dev->config;
	struct x430_servo_data *data = dev->data;
	const uint8_t code = cfg->factory_baud_code;
	const uint8_t rc = x430_device_write(&data->device, X430_ADDR_BAUD_RATE, 1U, &code, 0U);

	if (rc != DXL2_ERR_NONE) {
		LOG_ERR("factory Baud Rate %u: error 0x%02x", code, rc);
	}
	x430_device_flush_events(&data->device);
}

/* ------------------------------------------------------------------------ */

int x430_servo_start(const struct device *dev, const struct x430_servo_callbacks *callbacks,
		     void *user_data)
{
	const struct x430_servo_config *cfg = dev->config;
	struct x430_servo_data *data = dev->data;
	int ret;

	data->cb = callbacks;
	data->cb_data = user_data;
	data->next_poll = k_uptime_get() + CONFIG_SFS_DXL_POLL_MS;

	ret = uart_irq_callback_user_data_set(cfg->uart, uart_isr, data);
	if (ret != 0) {
		LOG_ERR("%s: interrupt callback (%d)", cfg->uart->name, ret);
		return ret;
	}
	uart_irq_rx_enable(cfg->uart);

	/* The tests wait for this line: the bus is ready when it appears */
	LOG_INF("DYNAMIXEL %s: ID %u, model %u, %s", CONFIG_X430_MODEL_NAME,
		x430_device_id(&data->device),
		(unsigned int)x430_device_get(&data->device, X430_ADDR_MODEL_NUMBER),
		cfg->uart->name);
	return 0;
}

int x430_servo_process(const struct device *dev, k_timeout_t timeout)
{
	struct x430_servo_data *data = dev->data;
	uint8_t chunk[64];
	uint32_t n;

	(void)k_sem_take(&data->rx_sem, timeout);

	while ((n = ring_buf_get(&data->rx_ring, chunk, sizeof(chunk))) > 0U) {
		x430_bus_receive(&data->bus, chunk, n);
	}

	if (data->factory_baud_pending) {
		x430_bus_lock(&data->bus);
		data->factory_baud_pending = false;
		apply_factory_baud(dev);
		x430_bus_unlock(&data->bus);
	}

	if (k_uptime_get() >= data->next_poll) {
		data->next_poll += CONFIG_SFS_DXL_POLL_MS;
		x430_bus_poll(&data->bus);

		const atomic_val_t overruns = atomic_set(&data->rx_overruns, 0);

		if (overruns != 0) {
			LOG_WRN("RX ring buffer overrun (%ld)", (long)overruns);
		}
	}
	return 0;
}

void x430_servo_lock(const struct device *dev)
{
	struct x430_servo_data *data = dev->data;

	x430_bus_lock(&data->bus);
}

void x430_servo_unlock(const struct device *dev)
{
	struct x430_servo_data *data = dev->data;

	x430_bus_unlock(&data->bus);
}

int32_t x430_servo_reg_get(const struct device *dev, uint16_t address)
{
	const struct x430_servo_data *data = dev->data;

	return x430_device_get(&data->device, address);
}

int x430_servo_reg_set(const struct device *dev, uint16_t address, int32_t value)
{
	struct x430_servo_data *data = dev->data;

	return x430_device_set(&data->device, address, value);
}

void x430_servo_raise_hw_error(const struct device *dev, uint8_t bits)
{
	struct x430_servo_data *data = dev->data;

	x430_device_raise_hardware_error(&data->device, bits);
}

static int x430_servo_init(const struct device *dev)
{
	const struct x430_servo_config *cfg = dev->config;
	struct x430_servo_data *data = dev->data;
	struct x430_bus_config bus_cfg = {
		.write = bus_write,
		.write_context = (void *)dev,
		.port = &data->port,
	};
	const struct x430_device_config dev_cfg = {
		.id = cfg->factory_id,
		.hooks = &hooks,
		.user_data = (void *)dev,
	};
	int ret;

	if (!device_is_ready(cfg->uart)) {
		LOG_ERR("%s not ready", cfg->uart->name);
		return -ENODEV;
	}

	k_mutex_init(&data->mutex);
	k_sem_init(&data->rx_sem, 0, 1);
	ring_buf_init(&data->rx_ring, sizeof(data->rx_storage), data->rx_storage);
	data->port = (struct x430_port){
		.context = &data->mutex,
		.lock = port_lock,
		.unlock = port_unlock,
		.millis = port_millis,
		.delay_us = port_delay_us,
	};

	ret = x430_nvm_init(&data->nvm, cfg->flash, cfg->flash_offset, cfg->flash_size);
	if (ret != 0) {
		LOG_WRN("NVM unavailable (%d): EEPROM area not persistent", ret);
	}

	if ((x430_bus_init(&data->bus, &bus_cfg) != X430_OK) ||
	    (x430_device_init(&data->device, &dev_cfg) != X430_OK) ||
	    (x430_bus_attach(&data->bus, &data->device) != X430_OK)) {
		LOG_ERR("X430 initialisation failed");
		return -EINVAL;
	}

	x430_bus_lock(&data->bus);
	data->baud = (uint8_t)x430_device_get(&data->device, X430_ADDR_BAUD_RATE);
	if (data->eeprom_missing && (data->baud != cfg->factory_baud_code)) {
		apply_factory_baud(dev); /* first start: this firmware's factory baud rate */
		data->baud = (uint8_t)x430_device_get(&data->device, X430_ADDR_BAUD_RATE);
	}
	(void)uart_set_baud(dev, data->baud);
	x430_bus_unlock(&data->bus);
	return 0;
}

#define STORAGE_NODE(n) DT_INST_PHANDLE(n, storage)

#define X430_SERVO_DEFINE(n)                                                                       \
	static const struct x430_servo_config x430_servo_config_##n = {                            \
		.uart = DEVICE_DT_GET(DT_INST_BUS(n)),                                             \
		.flash = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, storage),                            \
				     (PARTITION_NODE_DEVICE(STORAGE_NODE(n))), (NULL)),            \
		.flash_offset = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, storage),                     \
					    (PARTITION_NODE_OFFSET(STORAGE_NODE(n))), (0)),        \
		.flash_size = COND_CODE_1(DT_INST_NODE_HAS_PROP(n, storage),                       \
					  (PARTITION_NODE_SIZE(STORAGE_NODE(n))), (0)),            \
		.factory_id = DT_INST_PROP(n, factory_id),                                         \
		.factory_baud_code = DT_INST_PROP(n, factory_baud_code),                           \
	};                                                                                         \
	static struct x430_servo_data x430_servo_data_##n;                                         \
	DEVICE_DT_INST_DEFINE(n, x430_servo_init, NULL, &x430_servo_data_##n,                      \
			      &x430_servo_config_##n, POST_KERNEL,                                 \
			      CONFIG_X430_SERVO_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(X430_SERVO_DEFINE)
