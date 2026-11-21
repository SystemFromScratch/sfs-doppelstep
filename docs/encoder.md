# Encoder of the joint

With an encoder on the motor shaft, Doppelstep reports the measured position, detects
lost steps and corrects the position at standstill. The encoder is any Zephyr sensor with
the channel `SENSOR_CHAN_ROTATION`, chosen in the devicetree as `sfs,encoder`:

- on the full-bridge board, the MA730 on SPI2 ([Full-bridge board](hardware-full-bridge.md#encoder-ma730));
- in the simulation, `sfs,sim-encoder` on the motor model, fitted with `-sim_encoder`.

The B-G431B-ESC1 has no encoder. Without `sfs,encoder`, none of this is built and the
firmware behaves as before.

!!! warning "Status"
    Tested on the simulated motor (`tests/sil/test_encoder.py`) and in unit tests
    (`tests/app/encoder`). It has not run on the full-bridge board yet.

## Where it runs

The control task reads the encoder once per status publication (every millisecond by
default, `CONFIG_SFS_STATUS_PERIOD_MS`) and hands the reading to `app/src/enc_track.c`. That
module is pure logic without devices and decides what the motor driver should do. The
status queue carries the measured position, the lost-step counter and the encoder fault
along with the motor status (`struct app_status`), and `joint_bridge` maps them onto the
Control Table. The motor driver and the X430 driver do not change.

## Coordinates

The encoder gives 65536 counts per motor revolution. The tracker unwraps them over any
number of turns and converts them to microsteps (3200 per revolution for 200 × 16).

**At the start the angle is absolute.** The first reading, taken as a signed angle (half a
turn either way), becomes the position of the motor driver. Present Position after
power-on is therefore `start-position` plus the shaft angle, as on a real XM430, which knows
its angle after power-on. The encoder's zero is set with `zero-offset` of the MA730 node.
With a gearbox (`gear-motor-revs` ≠ `gear-joint-revs`) the angle is absolute only within
one motor revolution.

The MA730 gives valid data 260 ms after power-up. On the board MCUboot waits 500 ms for a
firmware update before the application starts, which covers it; an image flashed without
MCUboot could read the start angle too early.

After the start, the measured and the commanded position share one coordinate. Clear
Multi-turn shifts both together, by the same amount.

## Present Position and Position Trajectory

| Register | Without encoder | With encoder |
|---|---|---|
| Present Position (132) | Commanded position | **Measured** shaft position |
| Position Trajectory (140) | Commanded position | Commanded position (the field) |
| Present Velocity (128) | Commanded velocity | Commanded velocity (unchanged) |

The difference between Position Trajectory and Present Position is the load angle: under
a load the field leads the rotor, by up to a full step at the holding torque.

## Alignment at Torque Enable

A stepper has no absolute electrical angle. When the outputs come on, the rotor snaps to
the nearest equilibrium of the field, up to two full steps (3.6°) away, because the
electrical angle repeats every four full steps. The firmware handles it in three parts:

- **While the outputs are off**, the commanded position follows the shaft. If the joint is
  turned by hand or by gravity, the Goal Position that an XM430 copies from Present
  Position at Torque Enable means no move.
- **After Torque Enable**, the motor holds for `CONFIG_SFS_ENC_SETTLE_MS` (100 ms). Then
  the commanded position is set to the measured one. The field does not move; only its
  coordinate changes (Stepper-C keeps the electrical angle).
- **From then on**, the lost-step check and the regulator are armed. A move that starts
  before the settle time postpones the alignment to the next standstill.

The snap itself stays: the rotor moves by up to two full steps when the torque comes on,
as without an encoder. With the regulator on, it then returns to the Goal Position.
Avoiding the snap would need the electrical angle of the field to be set from the encoder,
which Stepper-C v1.5.0 cannot do yet.

## Lost steps

With the checks armed, `|commanded − measured| > CONFIG_SFS_ENC_LOST_STEP_USTEPS` (32
microsteps, two full steps) means the rotor has slipped to another equilibrium. Beyond
two full steps the torque of the field turns round. The firmware then:

- raises **Overload Error** (bit 5 of Hardware Error Status). It is in the factory
  Shutdown mask, so the emulator switches the torque off, as an XM430 that cannot hold its
  load. Reboot clears it;
- logs `lost steps: the rotor left the field (event N)`;
- arms the check again only after the next alignment.

A host that clears bit 5 in Shutdown (63) keeps the torque on: the motor then runs on,
and Present Position shows where the shaft really is.

## Encoder faults

Ten failed readings in a row latch an encoder fault. The firmware raises **Motor Encoder
Error** (bit 3), which is not in the factory Shutdown mask. From then on, Present Position
is the commanded position again, and the lost-step check and the regulator are off: the
motor goes on open loop.

## Position regulator

The field of a stepper is a stiff position source. The rotor follows it minus the load
angle (`T = Kt·I·sin(δ)`). The regulator therefore does not command a torque, as the PID of
an XM430 does. At standstill after a position move, it leads the field by a correction
until the measured position reaches the goal. Every `CONFIG_SFS_ENC_CORRECT_MS` (10 ms):

```text
correction += gain × (goal − measured),  |correction| ≤ CONFIG_SFS_ENC_CORRECTION_MAX_USTEPS (16)
field      = goal + correction
```

The gain is **Position P Gain (84) / 1024**: the factory value 800 gives 0.78, and the
error falls to about a fifth with every correction. P Gain 0 switches the regulator off.
Position I Gain (82) and D Gain (80) are stored but not used, because the integral action
lies in the correction itself.

- A correction is a move of a few microsteps. Moving (122) stays 0 meanwhile: it is no
  move of the host.
- The regulator acts only after a Goal Position move, not after Goal Velocity, a stop or
  the Bus Watchdog. During a move the stepper follows its trajectory without correction.
- The correction is limited to one full step, the load angle of the largest holding
  torque. A load beyond that is a lost step, not something to correct.

## Configuration

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_SFS_ENC_LOST_STEP_USTEPS` | 32 | Lost-step threshold [microsteps] |
| `CONFIG_SFS_ENC_SETTLE_MS` | 100 | Holding time after Torque Enable before the alignment |
| `CONFIG_SFS_ENC_CORRECT_MS` | 10 | Period of the regulator |
| `CONFIG_SFS_ENC_CORRECTION_MAX_USTEPS` | 16 | Largest lead of the regulator |
| Position P Gain (84) | 800 | Regulator gain × 1024, 0 = off |

## In the simulation

`app/boards/native_sim_native_64.overlay` chooses `sfs,sim-encoder` on the motor model. It
is fitted only with the command-line option `-sim_encoder`, so the other SIL tests run
open loop as before. `-sim_encoder_mount=<counts>` sets the reading at the start angle of
the rotor. The model log line ends with `shaft N usteps`, the rotor angle as an encoder
sees it.

The simulated encoder resolves one microstep of the model and has no filter. The MA730
resolves 14 bits and has a filter cutoff of about 23 Hz, with no lag at constant speed.
