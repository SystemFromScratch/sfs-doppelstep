# DYNAMIXEL emulation

To a host program, Doppelstep is a **DYNAMIXEL XM430-W350** (model number 1020) with one ID,
speaking Protocol 2.0. The X430 emulator validates every access exactly as the servo does:
ranges and limits, the EEPROM lock, Indirect Addresses, Fast Sync/Bulk Read, the Bus
Watchdog. The firmware then maps the registers onto the motor (`app/src/joint_bridge.c`).
This page lists that mapping, the units, and what differs from a real XM430.

## Register mapping

| Register | Mapping |
|---|---|
| Torque Enable (64) | `stepper_enable()` / `stepper_disable()`; refused (Result Fail) while the dead time is not set or with a fault |
| Operating Mode (11) | 3 position, 4 extended position, 1 velocity; 0, 5, 16 are refused (Data Range Error) |
| Goal Position (116) | `stepper_ctrl_move_to()`; limits from Profile Velocity/Acceleration (velocity- or time-based, Drive Mode bit 2), Velocity Limit and the `max-velocity` / `max-acceleration` properties of the motor node |
| Goal Velocity (104) | `stepper_ctrl_run()` or `stepper_ctrl_stop()` (velocity mode) |
| Drive Mode (10) bit 0, Homing Offset (20) | Direction and offset of the position mapping |
| Current Limit (38) | Run current = min(limit × 2.69 mA, `run-current-milliamp`) |
| Present Position (132), Position Trajectory (140) | Commanded position (no encoder) |
| Present Velocity (128), Velocity Trajectory (136) | Commanded velocity |
| Present Current (126) | Phase current amplitude √(iA² + iB²) |
| Present Input Voltage (144) | Measured VBUS (0.1 V) |
| Moving (122), Moving Status (123) | From the status of the motor driver |
| Hardware Error Status (70) | Over/undervoltage → Input Voltage; overcurrent, W trip, HAL → Electrical Shock |
| Bus Watchdog | `stepper_ctrl_stop()` |
| REBOOT / FACTORY_RESET | Outputs off, faults cleared; on the board (MCUboot) REBOOT then resets the MCU: silent for 500 ms plus startup, Present Position restarts ([Firmware update](firmware-update.md#dynamixel-reboot-on-the-board)) |
| CLEAR (multi-turn) | Position redefined within one turn |
| Baud Rate (8) | USART2 reconfigured after the status packet |

## Units

The conversions are in `app/src/joint.c`:

| Quantity | DYNAMIXEL unit | Motor side |
|---|---|---|
| Position | 4096 pulses per joint revolution | 3200 microsteps per motor revolution (200 × 16) |
| Velocity | 0.229 rev/min | microsteps per second |
| Acceleration | 214.577 rev/min² | microsteps per second² |
| Current | 2.69 mA | mA |
| Gear | `gear-motor-revs / gear-joint-revs` of the servo node: 1/1, direct drive | |

With the 17HS19-2004S1 in direct drive one joint revolution is one motor revolution:
4096 pulses = 3200 microsteps, so one pulse is 0.78 microsteps and one full step (1.8°) is
20.48 pulses. The largest velocity of the motor node, `max-velocity` 16000 microsteps/s, is
5 rev/s = 300 rev/min = 1310 × 0.229 rev/min. For a gearbox, set the two properties to its
ratio (motor revolutions per joint revolution): position, velocity and acceleration then
count at the joint.

## Startup position

The motor has no encoder. At power-on the rotor position is reported as Present Position
**2048** (`start-position` of the servo node, 0 rad for `dynamixel_hardware_interface`):
power the joint up in that position. With the torque off the rotor can be moved by hand, and
the reported position is then no longer the real one.

## Factory values and the EEPROM

| Value | Doppelstep | A real XM430 |
|---|---|---|
| ID | `factory-id` of the servo node, 1 | 1 |
| Baud rate | `factory-baud-code` 3 = 1 Mbps, the OpenMANIPULATOR-X bus speed | 57600 bps |
| Current Limit (38) | 557 = 1498 mA: `run-current-milliamp` 1500 mA of the motor node, rounded down | 1193 = 3209 mA |
| Velocity Limit (44) | 1023 = 234 rev/min: the motor could do 300 rev/min (1310), 1023 is the largest value of the X430 register | 200 = 45.8 rev/min |
| Max / Min Voltage Limit (32, 34) | 160 / 95 = 16 / 9.5 V, as an XM430 | 160 / 95 |

The factory values apply while no EEPROM block is stored and after FACTORY_RESET
(`x430_servo_set_factory_value()`, set by `app/src/joint_bridge.c` from the motor node).
A board with an EEPROM block from an older firmware keeps its stored values until a Factory
Reset.

The voltage limits stay those of the XM430, a 12 V servo: the register range ends at 16 V,
below the 24 V of this board. The emulator does not compare Present Input Voltage with them,
and the bus voltage protection uses `vbus-min-mv` / `vbus-max-mv` of the motor node
(10 / 28 V) instead. ID, Baud
Rate and the other EEPROM registers written over the bus are stored in flash (NVS, the
`storage` partition of the servo node) and survive a power cycle and a firmware update.
Flash writes are refused while the power stage is enabled: the MCU has a single flash bank,
and an erase stalls the CPU.

## Connecting a host program

| Host program | Port |
|---|---|
| On the board | ST-LINK Virtual COM Port (`COM5` on Windows, `/dev/ttyACM0` on Linux), 1 Mbps |
| `native_sim` on the same Linux machine or Dev Container | `build-native/ttyDXL` |
| `native_sim` in Docker, host program on WSL or another Linux machine | `python3 tools/sim/run_native.py connect --host localhost --port 5555` → `/tmp/ttyDXL` |
| `native_sim` in Docker, smoke test on Windows | `python tools\sim\dxl_smoke_test.py --tcp localhost:5555` |

For ROS 2, set the port of the `dynamixel_hardware_interface` description to `/tmp/ttyDXL`
(or `build-native/ttyDXL`); the baud rate is accepted and ignored on `native_sim`. The
pseudoterminal driver polls for the first byte of a packet every 10 ms, so an answer can take
up to about 10 ms (a real bus: well under 1 ms): keep the host's packet timeout above that.

## OpenMANIPULATOR-X

Its `ros2_control` description expects five XM430-W350 on one bus (IDs 11–15, 1 Mbps,
Operating Mode 3, Drive Mode 4 = time-based profile). One Doppelstep answers for **one** ID,
so over the Virtual COM Port the host sees one joint: set `factory-id` to the joint's ID (for
example 11). Fast Sync/Bulk Read answers come only from the IDs of one emulator bus.

The integration tests run this loop against the firmware with the official DynamixelSDK:
Sync Read of current, velocity and position, Sync Write of the goal, at 100 Hz
([Tests](testing.md)).

## Differences from a real XM430-W350

| Topic | Doppelstep |
|---|---|
| Position and velocity readings | Commanded values: there is no encoder, and lost steps are not detected yet |
| Operating Modes 0, 5, 16 | Refused with Data Range Error |
| Factory baud rate | 1 Mbps instead of 57600 bps |
| Factory Current Limit and Velocity Limit | Those of the real motor ([Factory values](#factory-values-and-the-eeprom)) |
| Max / Min Voltage Limit | 16 / 9.5 V as an XM430, not checked; the board runs at 24 V |
| Reboot on the board | Resets the MCU: 500 ms of silence plus startup, Present Position restarts at 2048 |
| Present Temperature | Keeps its table value: the NTC sensor is not converted yet, no overheating trip |
| Several IDs on one bus | One ID per board |

DYNAMIXEL and ROBOTIS are trademarks of ROBOTIS Co., Ltd. Doppelstep is an independent
project and is not affiliated with or endorsed by ROBOTIS.
