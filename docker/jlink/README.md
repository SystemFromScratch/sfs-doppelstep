# Optional SEGGER J-Link Software for the container

Put the SEGGER "J-Link Software and Documentation Pack" for **Linux, 64-bit,
TGZ archive** (`JLink_Linux_V<version>_x86_64.tgz`) into this folder before
building the image. Download it from https://www.segger.com/downloads/jlink/
(you accept SEGGER's license there). The archive is ignored by Git and is
never committed.

Needed only for the J-Link runner inside the container
(`west flash|debug|debugserver|attach|rtt -r jlink`, Cortex-Debug "J-Link in
container"). The J-Link GDB server on the Windows host and the OpenOCD runner
do not need it.
