/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * x430_config.h includes "x430_autoconf.h" when X430_HAS_AUTOCONF is defined.
 * In a Zephyr build the CONFIG_X430_* symbols come from Zephyr's Kconfig
 * (Kconfig.x430 is sourced by this module), so this header only forwards to
 * the generated Zephyr autoconf.h.
 */

#ifndef SFS_X430_AUTOCONF_H
#define SFS_X430_AUTOCONF_H

#include <zephyr/autoconf.h>

#endif /* SFS_X430_AUTOCONF_H */
