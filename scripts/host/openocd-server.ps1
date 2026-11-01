# SPDX-License-Identifier: Apache-2.0
#
# OpenOCD GDB server on the Windows host for the on-board ST-LINK/V2-1 of the
# B-G431B-ESC1 (original ST firmware). GDB in the dev container connects to
# host.docker.internal:3333 (launch configurations "... OpenOCD on host").
#
# Requires OpenOCD 0.12 or later on the host, for example the xPack build
# (https://xpack-dev-tools.github.io/openocd-xpack/), and the ST-LINK USB driver.
#
#   .\scripts\host\openocd-server.ps1
#   .\scripts\host\openocd-server.ps1 -OpenOcd C:\xpack-openocd\bin\openocd.exe -Rtt

param(
    [string]$OpenOcd = "openocd",
    [int]$GdbPort = 3333,
    [int]$TelnetPort = 4444,
    [switch]$Rtt,          # RTT log on TCP port $RttPort (telnet / PuTTY raw)
    [int]$RttPort = 9090
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$cfg = Join-Path $repo "boards\st\b_g431b_esc1\support\openocd.cfg"

$arguments = @(
    "-f", $cfg,
    # Listen on every interface: the container reaches the host through
    # host.docker.internal, not through 127.0.0.1 (Windows Firewall may ask).
    "-c", "bindto 0.0.0.0",
    "-c", "gdb_port $GdbPort",
    "-c", "telnet_port $TelnetPort",
    "-c", "tcl_port disabled",
    # Thread awareness (CONFIG_DEBUG_THREAD_INFO=y in app/prj.conf)
    "-c", '$_TARGETNAME configure -rtos Zephyr'
)

if ($Rtt) {
    $arguments += @(
        "-c", "init",
        "-c", 'rtt setup 0x20000000 0x8000 "SEGGER RTT"',
        "-c", "rtt start",
        "-c", "rtt server start $RttPort 0"
    )
}

Write-Host "OpenOCD: $OpenOcd $($arguments -join ' ')"
& $OpenOcd @arguments
