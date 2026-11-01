/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * DYNAMIXEL Protocol 2.0 transport: one emulated XM430-W350 (X430 Control
 * Table emulator) on the UART chosen as "sfs,dxl-uart", mapped onto the
 * Stepper-C motor (motor.h) through the unit conversions of joint.h.
 */

#ifndef SFS_DXL_BUS_H
#define SFS_DXL_BUS_H

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise NVM, the emulated device and the UART, and start the bus thread. */
int dxl_bus_init(void);

#ifdef __cplusplus
}
#endif

#endif /* SFS_DXL_BUS_H */
