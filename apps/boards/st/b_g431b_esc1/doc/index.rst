.. zephyr:board:: b_g431b_esc1

Overview
********

Out-of-tree Zephyr board for the STMicroelectronics B-G431B-ESC1 Discovery
kit: STM32G431CBU6 (Cortex-M4F, 170 MHz, 128 KiB flash, 32 KiB SRAM), three
half-bridges (L6387 gate drivers, STL180N6F7 MOSFETs), three low-side shunts
amplified by the internal OPAMPs, and a detachable ST-LINK/V2-1.

Pin assignment (UM2516 Rev 4, Table 4)
**************************************

============ ====================== =========================================
Pin          Function               Zephyr
============ ====================== =========================================
PA8/PC13     TIM1_CH1 / CH1N (U)    application (power stage node)
PA9/PA12     TIM1_CH2 / CH2N (V)    application (power stage node)
PA10/PB15    TIM1_CH3 / CH3N (W)    application (power stage node)
PA1/PA3/PA2  OPAMP1 VINP/VINM/VOUT  ``&opamp1``, ADC1_IN3
PA7/PA5/PA6  OPAMP2 VINP/VINM/VOUT  ``&opamp2``, ADC2_IN3
PB0/PB2/PB1  OPAMP3 VINP/VINM/VOUT  ``&opamp3``, ADC1_IN12
PA0          VBUS                   ADC1_IN1
PB14         Temperature (NTC)      ADC1_IN5 (not used yet)
PB12         Potentiometer          ADC1_IN11 (not used)
PB3/PB4      USART2 TX/RX (VCP)     ``&usart2``
PC6          STATUS LED             ``led0``
PC10         User button            ``sw0``
PA11/PB9     CAN RX/TX              not enabled
PB6/PB7/PB8  Encoder / Hall         not enabled
PA15         PWM input              not enabled
============ ====================== =========================================

Programming and debugging
*************************

``west flash`` / ``west debug`` use OpenOCD with the on-board ST-LINK/V2-1 by
default. With the ST-LINK converted to J-Link OB, use ``-r jlink``.
