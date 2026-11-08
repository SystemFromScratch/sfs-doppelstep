# License

Doppelstep (the application, the drivers, the board definition, the module glue, the
configuration, the tests, the host tools and this documentation) is licensed under the
**Apache License 2.0**. The full text is in the file `LICENSE` of the repository and at
[apache.org/licenses/LICENSE-2.0](https://www.apache.org/licenses/LICENSE-2.0).

In short, and without replacing the license text: you may use, copy, change and distribute
Doppelstep, also in commercial and closed-source products, provided that you keep the
copyright and license notices, state your changes, and include the license with what you
distribute. The license includes a patent grant and comes without warranty.

Copyright © 2026 Anton Vynohradov.

## Components under their own licenses

A build of Doppelstep includes or uses components that come under their own licenses. Check
them before you distribute a product.

| Component | Where it comes from |
|---|---|
| Zephyr RTOS, MCUboot, mbedtls, tf-psa-crypto, zcbor, CMSIS 6 | West projects imported from Zephyr v4.5.0 (`west.yml`) |
| STM32Cube HAL/LL (hal_stm32) | West project imported from Zephyr |
| SEGGER RTT | West project imported from Zephyr (`segger`) |
| Stepper-C, DYNAMIXEL X430 Control Table Emulator, DXL2-C, Stepper-Lab, sfs-stepper-power-spice | West projects (`west.yml`) |
| Zephyr SDK and the Python packages of the image | `Dockerfile` |
| Python packages of the GUI and the documentation | `requirements.txt` |

The SEGGER J-Link Software is not part of the repository or the image unless you add it
yourself under SEGGER's license (`docker/jlink/README.md`).

## Trademarks

The Apache License 2.0 grants no rights to trademarks. SystemFromScratch® is a registered
trademark; its name and logo may not be used to endorse or promote products derived from
Doppelstep without prior written permission. DYNAMIXEL and ROBOTIS are trademarks of
ROBOTIS Co., Ltd.; STM32 is a registered trademark of STMicroelectronics International N.V.
Doppelstep is not affiliated with or endorsed by them.
