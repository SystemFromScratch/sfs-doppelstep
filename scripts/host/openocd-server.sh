#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Linux/macOS host variant of openocd-server.ps1 (OpenOCD 0.12+ on the host).
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec "${OPENOCD:-openocd}" \
    -f "${repo}/apps/boards/st/b_g431b_esc1/support/openocd.cfg" \
    -c "bindto 0.0.0.0" -c "gdb_port ${GDB_PORT:-3333}" -c "tcl_port disabled" \
    -c '$_TARGETNAME configure -rtos Zephyr'
