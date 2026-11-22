.. zephyr:board:: sfs_doppelstep_full_bridge

Overview
********

Out-of-tree Zephyr board for the Stepper Motor FOC Closed-Loop Driver of
Awesome-SMT-Machine (https://github.com/baoqi-zhong/Awesome-SMT-Machine), written
from its published schematic (KiCad 8): STM32G431CBU6 (Cortex-M4F, 170 MHz from an
8 MHz crystal, 128 KiB flash, 32 KiB SRAM), TI DRV8412 (two full H-bridges, mode
M3..M1 = 000), 10 mOhm low-side shunts amplified by OPAMP1/2 with external feedback,
MPS MA730 magnetic encoder on SPI2, eight WS2812B-2020, NTC, TJA1042 CAN transceiver,
24 V supply. The board mounts on the back of a NEMA 17 motor.

Origin and terms of use of the board, motor wiring, scaling and the bring-up of
Doppelstep on it: https://systemfromscratch.github.io/sfs-doppelstep/hardware-full-bridge/.
Built and tested in CI, not yet run on hardware.

Pin assignment (schematic)
**************************

============ ====================== =========================================
Pin          Function               Zephyr
============ ====================== =========================================
PA8..PA11    TIM1_CH1..CH4          application (power stage node):
             (PWM_A..PWM_D)         legs A+, A-, B+, B-
PC6 / PA12   RESET_AB / RESET_CD    application (power stage node)
PB11 / PB0   FAULT / OTW            application (power stage node)
PA1/PA3/PA2  OPAMP1 + / - / out     ``&opamp1``, ADC1_IN3 (phase A)
PA7/PA5/PA6  OPAMP2 + / - / out     ``&opamp2``, ADC2_IN3 (phase B)
PA0          VBUS (100k / 10k)      ADC1_IN1, COMP3 (overvoltage trip)
PA4          Temperature (NTC)      ADC2_IN17, power stage node
PB12..PB15   SPI2 CS/SCK/MISO/MOSI  ``&spi2``, ``encoder`` (``sfs,ma730``)
PC10/PC11    USART3 TX/RX (J4)      ``&usart3`` = ``sfs_dxl_uart`` (bus)
PB3          WS2812B data           ``leds`` (``sfs,ws2812-stm32-tim``,
                                    TIM2_CH2 + DMA1), alias ``led-strip``
PA15         BTN1                   ``sw0``
PB8/PB9      FDCAN1 RX/TX (J6, J7)  not enabled
PA13/PA14    SWD (J3)               debug
============ ====================== =========================================

The power stage, the motor and the servo are application nodes
(``app/boards/sfs_doppelstep_full_bridge.overlay``). TIM1, TIM2, ADC1, ADC2, DMA1 and
DMAMUX1 stay ``disabled``: the Doppelstep drivers program them directly.

Flash layout
************

===================== ======== ===============================================
Partition             Size     Content
===================== ======== ===============================================
``boot_partition``    38 KiB   MCUboot (sysbuild)
``slot0_partition``   86 KiB   application, signed; serial recovery writes it
``storage_partition`` 4 KiB    NVS: EEPROM area of the emulated DYNAMIXEL
===================== ======== ===============================================

Programming and debugging
*************************

``west flash`` / ``west debug`` use OpenOCD with an external ST-LINK on the SWD
connector J3 (``support/openocd.cfg``). J3 has no NRST: OpenOCD resets through
SYSRESETREQ. ``-r jlink``, ``-r pyocd`` and ``-r stm32cubeprogrammer`` are configured too.
