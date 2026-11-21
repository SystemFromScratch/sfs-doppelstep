# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(ma730.c)
zephyr_library_sources_ifdef(CONFIG_MA730_EMUL ma730_emul.c)
