# SPDX-License-Identifier: Apache-2.0
#
# SEGGER J-Link Remote Server on the Windows host: makes the J-Link reachable
# over TCP/IP, so that the J-Link runner INSIDE the container (J-Link Software
# from docker/jlink/) can use it without USB pass-through:
#
#   host:       .\scripts\host\jlink-remote-server.ps1
#   container:  west debug -d build -r jlink --dev-id <host IP> --dev-id-type ip
#               west debugserver / flash / rtt: same options
#
# <host IP>: getent hosts host.docker.internal (the J-Link -select option
# expects an IP address). Not yet verified with the J-Link OB firmware of the
# ST-LINK; the host GDB server (jlink-gdb-server.ps1) is the tested path.

param(
    [string]$JLinkDir = "C:\Program Files\SEGGER\JLink"
)

$ErrorActionPreference = "Stop"
$server = Join-Path $JLinkDir "JLinkRemoteServerCL.exe"

Write-Host "J-Link Remote Server: $server (port 19020)"
& $server
