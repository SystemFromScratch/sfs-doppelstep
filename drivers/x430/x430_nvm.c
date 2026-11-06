/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "x430_nvm.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(x430_nvm, CONFIG_X430_SERVO_LOG_LEVEL);

#if defined(CONFIG_SFS_DXL_NVM)

#include <zephyr/drivers/flash.h>

#define NVM_ID_BASE 1U /* NVS id = slot + NVM_ID_BASE */

int x430_nvm_init(struct x430_nvm *nvm, const struct device *flash, off_t offset, size_t size)
{
	struct flash_pages_info page;
	int ret;

	if ((flash == NULL) || !device_is_ready(flash)) {
		return -ENODEV;
	}
	ret = flash_get_page_info_by_offs(flash, offset, &page);
	if (ret != 0) {
		return ret;
	}
	nvm->fs.flash_device = flash;
	nvm->fs.offset = offset;
	nvm->fs.sector_size = page.size;
	nvm->fs.sector_count = (uint16_t)(size / page.size);

	ret = nvs_mount(&nvm->fs);
	if (ret != 0) {
		LOG_ERR("nvs_mount: %d", ret);
		return ret;
	}
	nvm->mounted = true;
	LOG_INF("NVS: %u sectors of %u bytes", nvm->fs.sector_count, nvm->fs.sector_size);
	return 0;
}

int x430_nvm_load(struct x430_nvm *nvm, uint8_t slot, uint8_t *data, size_t size)
{
	ssize_t rc;

	if (!nvm->mounted) {
		return -ENODEV;
	}
	rc = nvs_read(&nvm->fs, (uint16_t)(slot + NVM_ID_BASE), data, size);
	return (rc == (ssize_t)size) ? 0 : -ENOENT;
}

int x430_nvm_store(struct x430_nvm *nvm, uint8_t slot, const uint8_t *data, size_t size)
{
	ssize_t rc;

	if (!nvm->mounted) {
		return -ENODEV;
	}
	rc = nvs_write(&nvm->fs, (uint16_t)(slot + NVM_ID_BASE), data, size);
	if (rc < 0) {
		LOG_ERR("nvs_write slot %u: %d", slot, (int)rc);
		return (int)rc;
	}
	return 0;
}

#else /* CONFIG_SFS_DXL_NVM */

int x430_nvm_init(struct x430_nvm *nvm, const struct device *flash, off_t offset, size_t size)
{
	ARG_UNUSED(nvm);
	ARG_UNUSED(flash);
	ARG_UNUSED(offset);
	ARG_UNUSED(size);
	return 0;
}

int x430_nvm_load(struct x430_nvm *nvm, uint8_t slot, uint8_t *data, size_t size)
{
	ARG_UNUSED(nvm);
	ARG_UNUSED(slot);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	return -ENOTSUP;
}

int x430_nvm_store(struct x430_nvm *nvm, uint8_t slot, const uint8_t *data, size_t size)
{
	ARG_UNUSED(nvm);
	ARG_UNUSED(slot);
	ARG_UNUSED(data);
	ARG_UNUSED(size);
	return -ENOTSUP;
}

#endif /* CONFIG_SFS_DXL_NVM */
