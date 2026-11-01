# SPDX-License-Identifier: Apache-2.0
#
# SEGGER J-Link GDB Server on the Windows host, for the on-board ST-LINK of the
# B-G431B-ESC1 converted to J-Link OB (or an external J-Link on the SWD pads).
# GDB in the dev container connects to host.docker.internal:2331 (launch
# configurations "... J-Link GDB Server on host"). RTT: telnet port 19021.
#
#   .\scripts\host\jlink-gdb-server.ps1
#   .\scripts\host\jlink-gdb-server.ps1 -JLinkDir "C:\Program Files\SEGGER\JLink_V812"

param(
    [string]$JLinkDir = "C:\Program Files\SEGGER\JLink",
    [int]$Port = 2331,
    [int]$Speed = 4000
)

$ErrorActionPreference = "Stop"
$server = Join-Path $JLinkDir "JLinkGDBServerCL.exe"

$arguments = @(
    "-device", "STM32G431CB",
    "-if", "SWD",
    "-speed", "$Speed",
    "-port", "$Port",
    # Accept connections from the container (host.docker.internal)
    "-nolocalhostonly"
)

Write-Host "J-Link GDB Server: $server $($arguments -join ' ')"
& $server @arguments
