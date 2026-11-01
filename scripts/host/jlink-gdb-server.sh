#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Linux/macOS host variant of jlink-gdb-server.ps1.
set -euo pipefail
exec "${JLINK_GDB_SERVER:-JLinkGDBServerCLExe}" \
    -device STM32G431CB -if SWD -speed "${SPEED:-4000}" -port "${PORT:-2331}" -nolocalhostonly
