# SPDX-License-Identifier: Apache-2.0
"""Test scenarios on the board: the register checks of the SIL tests (tests/sil).

Every scenario repeats one SIL test with the same writes, waits and limits. On
native_sim the tests also read the log of the motor model (rotor, lag, phase
currents of the model); the board has no such log, so those checks are left
out and listed in `omitted`. The protection tests (load torque, supply faults)
need the fault to be made on the bench and are not part of this list.

Where the board differs from native_sim on purpose (DYNAMIXEL Reboot resets the
MCU: docs/firmware-update.md), the scenario checks the board behaviour and its
`board_note` says so.

Like a fresh native_sim process for every SIL test, every scenario starts with a
DYNAMIXEL Reboot, which resets the MCU on the board, and ends with Torque
Enable 0. Scenarios that change the EEPROM put the factory values back.
"""

import time
from dataclasses import dataclass, field

from . import protocol as p
from . import registers as r

DXL_ID = 1                        # tests/support.py
START_POSITION = 2048             # start-position of the servo node (sfs,x430-servo.yaml)
CURRENT_UNIT_MA = r.MA_PER_UNIT
RUN_MA = 1500                     # run-current-milliamp (app/boards/b_g431b_esc1.overlay)
HOLD_MA = 500                     # hold-current-milliamp
RESTART_TIMEOUT = 5.0             # tool timeout: device answers again after a Reboot

PASS, FAIL, ERROR, STOPPED = "PASS", "FAIL", "ERROR", "STOPPED"

# docs/hardware-checklist.md, check 5
DEAD_TIME_HINT = ("Torque Enable refused (Result Fail). A fault is latched (Hardware Error "
                  "Status not 0: Reboot clears it), the self-test of the bridge and the motor "
                  "failed (RTT log 'selftest: ...', Electrical Shock), the supply is outside "
                  "10..28 V (Input Voltage Error), or the firmware refuses the outputs because "
                  "dead-time-ns is 0 in app/boards/b_g431b_esc1.overlay (docs/protection.md)")


class CheckFailed(Exception):
    pass


class Stopped(Exception):
    pass


@dataclass
class Check:
    name: str
    expected: str
    measured: str
    ok: bool | None               # None: recorded value without a criterion
    note: str = ""


@dataclass
class Scenario:
    id: str
    title: str
    source: str                   # SIL test it repeats
    run: object                   # function(ctx)
    moves: bool = False           # turns the motor
    eeprom: str = ""              # what it changes in the EEPROM (restored at the end)
    omitted: str = ""             # SIL checks left out (model log)
    board_note: str = ""          # intended difference from native_sim


@dataclass
class ScenarioResult:
    scenario: Scenario
    status: str = PASS
    checks: list = field(default_factory=list)
    error: str = ""
    seconds: float = 0.0


class Context:
    """What a scenario uses: the bus, checks, waits."""

    def __init__(self, bus, result, stop_event, log=print):
        self.bus = bus
        self.result = result
        self.stop_event = stop_event
        self.log = log

    # --- recording --------------------------------------------------------------

    def expect(self, name, measured, expected, ok, note=""):
        """Record a check; a failed check ends the scenario (as an assert in pytest)."""
        self.result.checks.append(Check(name, expected, str(measured), bool(ok), note))
        self.log(f"  {'ok  ' if ok else 'FAIL'} {name}: {measured} (expected {expected})")
        if not ok:
            raise CheckFailed(f"{name}: {measured}, expected {expected}" +
                              (f". {note}" if note else ""))

    def info(self, name, measured, note=""):
        self.result.checks.append(Check(name, "no criterion", str(measured), None, note))
        self.log(f"  info {name}: {measured}")

    def expect_error(self, name, func, error):
        """`func()` must be refused with the status error `error`."""
        try:
            func()
        except p.DxlError as exc:
            self.expect(name, p.ERROR_NAMES.get(exc.error, exc.error), p.ERROR_NAMES[error],
                        exc.error == error)
            return
        self.expect(name, "accepted", p.ERROR_NAMES[error], False)

    def expect_no_answer(self, name, func):
        try:
            func()
        except TimeoutError:
            self.expect(name, "no answer", "no answer", True)
            return
        self.expect(name, "answered", "no answer", False)

    # --- waits ------------------------------------------------------------------

    def sleep(self, seconds):
        if self.stop_event.wait(seconds):
            raise Stopped()

    def wait_until(self, predicate, timeout, interval=0.05, what="condition"):
        """Poll `predicate` until it returns a truthy value (tests/support.py)."""
        deadline = time.monotonic() + timeout
        while True:
            value = predicate()
            if value:
                return value
            if time.monotonic() > deadline:
                self.expect(what, f"not within {timeout} s", f"within {timeout} s", False)
            self.sleep(interval)

    # --- helpers of the SIL tests ---------------------------------------------------

    def read(self, reg, dxl_id=DXL_ID):
        return self.bus.read(dxl_id, reg)

    def write(self, reg, value, dxl_id=DXL_ID):
        return self.bus.write(dxl_id, reg, value)

    def torque_on(self):
        try:
            self.write(r.TORQUE_ENABLE, 1)
        except p.DxlError as exc:
            if exc.error == p.ERR_RESULT_FAIL:
                self.expect("Torque Enable 1", "Result Fail", "accepted", False, DEAD_TIME_HINT)
            raise

    def move_and_wait(self, goal, timeout=5.0):
        """tests/sil/test_motion.py: move_and_wait()."""
        self.write(r.GOAL_POSITION, goal)
        self.wait_until(lambda: self.read(r.MOVING) == 1 or
                        self.read(r.PRESENT_POSITION) == goal, 1.0, what="move start")
        self.wait_until(lambda: self.read(r.MOVING) == 0, timeout, what="move end")

    def restart(self, dxl_id=DXL_ID):
        """DYNAMIXEL Reboot and wait until the device answers again; seconds waited."""
        self.bus.reboot(dxl_id)
        _model, _fw, waited = self.bus.wait_for_ping(dxl_id, RESTART_TIMEOUT)
        return waited


def approx(value, target, tol):
    return abs(value - target) <= tol


# --- tests/sil/test_boot.py ---------------------------------------------------------

def boot_ping(ctx):
    model, firmware = ctx.bus.ping(DXL_ID)
    ctx.expect("Model Number", model, "1020 (XM430-W350)", model == 1020)
    ctx.expect("Firmware Version", firmware, "> 0", firmware > 0)


def boot_live_registers(ctx):
    volts = ctx.read(r.PRESENT_INPUT_VOLTAGE)
    ctx.info("Present Input Voltage", f"{volts / 10:.1f} V",
             "SIL expects 24.0 V, the supply of the model; compare with a multimeter "
             "(docs/hardware-checklist.md, check 6)")
    for reg, value in ((r.PRESENT_POSITION, START_POSITION), (r.PRESENT_CURRENT, 0),
                       (r.PRESENT_VELOCITY, 0), (r.MOVING, 0), (r.TORQUE_ENABLE, 0),
                       (r.HARDWARE_ERROR_STATUS, 0)):
        measured = ctx.read(reg)
        ctx.expect(reg.name, measured, value, measured == value)


def boot_factory_eeprom(ctx):
    for reg, value, text in ((r.ID, 1, "1"), (r.BAUD_RATE, 3, "3 (1 Mbps)"),
                             (r.OPERATING_MODE, 3, "3 (position control)")):
        measured = ctx.read(reg)
        ctx.expect(reg.name, measured, text, measured == value)


# --- tests/sil/test_bus.py -----------------------------------------------------------

def bus_bad_crc_and_other_ids(ctx):
    bad = bytearray(p.packet(DXL_ID, p.INST_PING))
    bad[-1] ^= 0xFF
    ctx.bus.flush()
    ctx.bus.write_raw(bytes(bad))
    answer = ctx.bus.receive(0.3)
    ctx.expect("Ping with a wrong CRC", "answered" if answer else "no answer", "no answer",
               answer is None)
    ctx.bus.write_raw(p.packet(9, p.INST_PING))
    answer = ctx.bus.receive(0.3)
    ctx.expect("Ping to ID 9", "answered" if answer else "no answer", "no answer",
               answer is None)
    model = ctx.bus.ping(DXL_ID)[0]
    ctx.expect("Ping to ID 1 afterwards", model, 1020, model == 1020)


def bus_watchdog(ctx):
    try:
        ctx.write(r.OPERATING_MODE, 1)                   # velocity control
        ctx.torque_on()
        ctx.write(r.BUS_WATCHDOG, 25)                    # 500 ms
        ctx.write(r.GOAL_VELOCITY, 100)
        ctx.sleep(0.15)
        ctx.sleep(1.0)                                   # host silent
        value = ctx.read(r.BUS_WATCHDOG)
        ctx.expect("Bus Watchdog after 1 s of silence", value, "-1 (watchdog error)",
                   value == -1)
        ctx.expect_error("Goal Velocity while the watchdog reports an error",
                         lambda: ctx.write(r.GOAL_VELOCITY, 50), p.ERR_DATA_RANGE)
        ctx.write(r.BUS_WATCHDOG, 0)
        ctx.write(r.GOAL_VELOCITY, 50)
        ctx.expect("Goal Velocity after clearing the watchdog", "accepted", "accepted", True)
    finally:
        _torque_off(ctx)
        _restore(ctx, r.OPERATING_MODE, 3)


def bus_reboot(ctx):
    ctx.torque_on()
    ctx.write(r.GOAL_POSITION, START_POSITION + 512)
    ctx.wait_until(lambda: ctx.read(r.PRESENT_POSITION) == START_POSITION + 512, 3.0,
                   what="move end")
    waited = ctx.restart()
    ctx.info("Answer after the Reboot", f"{waited:.2f} s",
             "MCUboot window 500 ms plus the startup (docs/firmware-update.md)")
    torque = ctx.read(r.TORQUE_ENABLE)
    ctx.expect("Torque Enable after the Reboot", torque, 0, torque == 0)
    position = ctx.read(r.PRESENT_POSITION)
    ctx.expect("Present Position after the Reboot", position,
               f"{START_POSITION} (restarts on the board)", position == START_POSITION)


def bus_factory_reset(ctx):
    try:
        ctx.write(r.ID, 5)
        model = ctx.bus.ping(5)[0]
        ctx.expect("Ping to the new ID 5", model, 1020, model == 1020)
        ctx.bus.factory_reset(5, 0xFF)
        ctx.sleep(0.3)
        model = ctx.bus.ping(DXL_ID)[0]
        ctx.expect("Ping to ID 1 after Factory Reset", model, 1020, model == 1020)
        ctx.expect_no_answer("Ping to ID 5 after Factory Reset",
                             lambda: ctx.bus.ping(5, timeout=0.2))
    finally:
        try:
            ctx.bus.ping(DXL_ID, timeout=0.2)
        except TimeoutError:
            _restore(ctx, r.ID, DXL_ID, 5)


def bus_sync(ctx):
    ctx.torque_on()
    ctx.bus.sync_write(r.GOAL_POSITION, {DXL_ID: START_POSITION - 512})
    ctx.wait_until(lambda: ctx.bus.sync_read([DXL_ID], r.PRESENT_POSITION)[DXL_ID] ==
                   START_POSITION - 512, 3.0, what="sync read of the goal")
    ctx.expect("Sync Read of Present Position", START_POSITION - 512,
               START_POSITION - 512, True)


def bus_rapid_goals(ctx):
    ctx.torque_on()
    answered = 0
    try:
        for k in range(200):
            ctx.write(r.GOAL_POSITION, START_POSITION + (k % 2) * 512)
            answered += 1
    except TimeoutError:
        pass
    ctx.expect("Goal Position writes answered", f"{answered} of 200", "200 of 200",
               answered == 200)


def bus_clear_multi_turn(ctx):
    ctx.torque_on()
    ctx.write(r.GOAL_POSITION, START_POSITION + 1024)
    ctx.sleep(0.05)
    ctx.expect_error("Clear multi-turn during a move", lambda: ctx.bus.clear_multi_turn(DXL_ID),
                     p.ERR_RESULT_FAIL)
    ctx.wait_until(lambda: ctx.read(r.PRESENT_POSITION) == START_POSITION + 1024, 5.0,
                   what="the move continues to its goal")
    ctx.expect("Present Position", START_POSITION + 1024, START_POSITION + 1024, True)


def bus_garbage(ctx):
    ctx.bus.write_raw(bytes(range(256)) * 8)
    ctx.sleep(0.1)
    ctx.bus.flush()
    model = ctx.bus.ping(DXL_ID)[0]
    ctx.expect("Ping after 2 KiB of garbage", model, 1020, model == 1020)


# --- tests/sil/test_eeprom.py --------------------------------------------------------

def eeprom_survives_restart(ctx):
    current_id = DXL_ID
    try:
        ctx.write(r.DRIVE_MODE, 4)                       # EEPROM
        ctx.write(r.ID, 7)                               # EEPROM, last: the ID changes
        current_id = 7
        ctx.write(r.PROFILE_VELOCITY, 123, dxl_id=7)     # RAM
        ctx.restart(7)
        model = ctx.bus.ping(7)[0]
        ctx.expect("Ping to ID 7 after the restart", model, 1020, model == 1020)
        drive = ctx.read(r.DRIVE_MODE, dxl_id=7)
        ctx.expect("Drive Mode (EEPROM)", drive, 4, drive == 4)
        velocity = ctx.read(r.PROFILE_VELOCITY, dxl_id=7)
        ctx.expect("Profile Velocity (RAM)", velocity, 0, velocity == 0)
    finally:
        _restore(ctx, r.DRIVE_MODE, 0, current_id)
        if current_id != DXL_ID:
            _restore(ctx, r.ID, DXL_ID, current_id)


# --- tests/sil/test_motion.py --------------------------------------------------------

def motion_goal_position(ctx):
    ctx.torque_on()
    goal = START_POSITION + 1024                         # +90 degrees
    ctx.move_and_wait(goal)
    position = ctx.read(r.PRESENT_POSITION)
    ctx.expect("Present Position", position, goal, position == goal)
    status = ctx.read(r.MOVING_STATUS)
    ctx.expect("Moving Status bit 0 (in position)", f"0x{status:02x}", "bit 0 set",
               status & 0x01)


def motion_run_hold_current(ctx):
    ctx.torque_on()
    ctx.move_and_wait(START_POSITION + 512)
    run = ctx.read(r.PRESENT_CURRENT) * CURRENT_UNIT_MA
    ctx.expect("Present Current after the move (run)", f"{run:.0f} mA", f"{RUN_MA} ± 40 mA",
               approx(run, RUN_MA, 40))
    ctx.sleep(0.8)                                       # hold-delay-ms = 500
    hold = ctx.read(r.PRESENT_CURRENT) * CURRENT_UNIT_MA
    ctx.expect("Present Current after 0.8 s (hold)", f"{hold:.0f} mA", f"{HOLD_MA} ± 40 mA",
               approx(hold, HOLD_MA, 40))


def motion_profile_velocity(ctx):
    ctx.write(r.PROFILE_VELOCITY, 200)                   # 45.8 rev/min
    ctx.torque_on()
    ctx.write(r.GOAL_POSITION, START_POSITION + 2000)
    peak = ctx.wait_until(lambda: (v := ctx.read(r.PRESENT_VELOCITY)) >= 199 and v, 2.0,
                          what="cruise speed")
    ctx.expect("Present Velocity at cruise", peak, "199 .. 201", peak <= 201)
    ctx.wait_until(lambda: ctx.read(r.MOVING) == 0, 3.0, what="move end")


def motion_time_profile(ctx):
    try:
        ctx.write(r.DRIVE_MODE, 4)                       # time-based profile
        ctx.write(r.PROFILE_VELOCITY, 1000)              # whole move [ms]
        ctx.write(r.PROFILE_ACCELERATION, 250)           # acceleration time [ms]
        ctx.torque_on()
        t0 = ctx.read(r.REALTIME_TICK)
        ctx.write(r.GOAL_POSITION, START_POSITION + 1024)
        ctx.wait_until(lambda: ctx.read(r.MOVING) == 1, 0.5, interval=0.01, what="move start")
        ctx.wait_until(lambda: ctx.read(r.MOVING) == 0, 3.0, interval=0.01, what="move end")
        elapsed_ms = (ctx.read(r.REALTIME_TICK) - t0) % 32768
        ctx.expect("Move time (Realtime Tick)", f"{elapsed_ms / 1000:.3f} s", "1.000 ± 0.150 s",
                   approx(elapsed_ms / 1000, 1.0, 0.15))
    finally:
        _torque_off(ctx)
        _restore(ctx, r.DRIVE_MODE, 0)


def motion_velocity_mode(ctx):
    try:
        ctx.write(r.OPERATING_MODE, 1)
        ctx.torque_on()
        ctx.write(r.GOAL_VELOCITY, -100)                 # -22.9 rev/min
        ctx.wait_until(lambda: ctx.read(r.PRESENT_VELOCITY) == -100, 2.0, what="goal velocity")
        ctx.expect("Present Velocity", -100, -100, True)
        ctx.sleep(0.3)
        ctx.write(r.GOAL_VELOCITY, 0)
        ctx.wait_until(lambda: ctx.read(r.MOVING) == 0, 2.0, what="stop")
        ctx.expect("Moving after Goal Velocity 0", 0, 0, True)
    finally:
        _torque_off(ctx)
        _restore(ctx, r.OPERATING_MODE, 3)


def motion_torque_off(ctx):
    ctx.torque_on()
    ctx.move_and_wait(START_POSITION + 256)
    ctx.write(r.TORQUE_ENABLE, 0)
    ctx.sleep(0.15)
    current = ctx.read(r.PRESENT_CURRENT)
    ctx.expect("Present Current with the torque off", current, 0, current == 0)


def motion_unsupported_mode(ctx):
    ctx.expect_error("Operating Mode 16 (PWM control)",
                     lambda: ctx.write(r.OPERATING_MODE, 16), p.ERR_DATA_RANGE)
    mode = ctx.read(r.OPERATING_MODE)
    ctx.expect("Operating Mode", mode, 3, mode == 3)


def motion_eeprom_locked(ctx):
    ctx.torque_on()
    ctx.expect_error("Operating Mode write with the torque on",
                     lambda: ctx.write(r.OPERATING_MODE, 1), p.ERR_ACCESS)


def _restore(ctx, reg, value, dxl_id=DXL_ID):
    """Put an EEPROM value back; a failure is logged, the scenario result stays."""
    try:
        ctx.write(reg, value, dxl_id=dxl_id)
    except (TimeoutError, IOError, p.DxlError) as exc:
        ctx.log(f"  could not restore {reg.name} = {value} at ID {dxl_id}: {exc}")


def _torque_off(ctx):
    try:
        ctx.write(r.TORQUE_ENABLE, 0)
    except (TimeoutError, p.DxlError):
        pass


S = Scenario
MODEL_LOG = "checks on the log of the motor model"
SCENARIOS = [
    S("boot-ping", "Ping answers as XM430-W350",
      "tests/sil/test_boot.py::test_ping_answers_as_xm430_w350", boot_ping),
    S("boot-live", "Live registers at rest",
      "tests/sil/test_boot.py::test_live_registers_at_rest", boot_live_registers,
      board_note="Present Input Voltage is the bench supply: recorded, not checked"),
    S("boot-eeprom", "Factory EEPROM (ID 1, 1 Mbps, position control)",
      "tests/sil/test_boot.py::test_factory_eeprom", boot_factory_eeprom),
    S("bus-crc", "Bad CRC and other IDs get no answer",
      "tests/sil/test_bus.py::test_bad_crc_and_other_ids_get_no_answer",
      bus_bad_crc_and_other_ids),
    S("bus-watchdog", "Bus Watchdog stops the motor",
      "tests/sil/test_bus.py::test_bus_watchdog_stops_the_motor", bus_watchdog, moves=True,
      eeprom="Operating Mode (back to 3)", omitted="rotor speed and standstill of the model"),
    S("bus-reboot", "Reboot disables the torque",
      "tests/sil/test_bus.py::test_reboot_disables_the_torque_and_keeps_the_position",
      bus_reboot, moves=True,
      board_note="on the board Reboot resets the MCU, so Present Position restarts at 2048 "
                 "instead of being kept (docs/firmware-update.md)"),
    S("bus-factory", "Factory Reset restores the ID",
      "tests/sil/test_bus.py::test_factory_reset_restores_the_id", bus_factory_reset,
      eeprom="all EEPROM registers back to the factory values"),
    S("bus-sync", "Sync Write and Sync Read",
      "tests/sil/test_bus.py::test_sync_write_and_sync_read", bus_sync, moves=True),
    S("bus-rapid", "200 Goal Position writes during a move all get an answer",
      "tests/sil/test_bus.py::test_rapid_goals_during_a_move_all_get_an_answer",
      bus_rapid_goals, moves=True, omitted="errors in the firmware log"),
    S("bus-clear", "Clear multi-turn during a move is rejected",
      "tests/sil/test_bus.py::test_clear_multi_turn_during_a_move_is_rejected",
      bus_clear_multi_turn, moves=True),
    S("bus-garbage", "Garbage on the bus does not stop it",
      "tests/sil/test_bus.py::test_garbage_on_the_bus_does_not_stop_it", bus_garbage,
      omitted="errors in the firmware log"),
    S("eeprom-restart", "EEPROM survives a restart, RAM does not",
      "tests/sil/test_eeprom.py::test_eeprom_survives_a_restart_ram_does_not",
      eeprom_survives_restart, eeprom="Drive Mode and ID (back to 0 and 1)",
      board_note="restart = DYNAMIXEL Reboot, which resets the MCU on the board; "
                 "the SIL test restarts the native_sim process"),
    S("motion-goal", "Goal Position moves the joint",
      "tests/sil/test_motion.py::test_goal_position_moves_the_rotor", motion_goal_position,
      moves=True, omitted="rotor position and lag of the model"),
    S("motion-current", "Run current, then hold current",
      "tests/sil/test_motion.py::test_run_then_hold_current", motion_run_hold_current,
      moves=True, omitted="phase currents of the model"),
    S("motion-velocity", "Profile Velocity sets the speed",
      "tests/sil/test_motion.py::test_profile_velocity_sets_the_speed",
      motion_profile_velocity, moves=True, omitted="rotor speed of the model"),
    S("motion-time", "Time-based profile keeps the time",
      "tests/sil/test_motion.py::test_time_based_profile_keeps_the_time", motion_time_profile,
      moves=True, eeprom="Drive Mode (back to 0)", omitted="rotor position of the model"),
    S("motion-velmode", "Velocity control mode",
      "tests/sil/test_motion.py::test_velocity_mode", motion_velocity_mode, moves=True,
      eeprom="Operating Mode (back to 3)", omitted="rotor speed, lag and standstill of the model"),
    S("motion-torqueoff", "Torque off releases the motor",
      "tests/sil/test_motion.py::test_torque_off_releases_the_motor", motion_torque_off,
      moves=True, omitted="phase currents of the model"),
    S("motion-mode16", "Unsupported Operating Mode is refused",
      "tests/sil/test_motion.py::test_unsupported_operating_mode_is_refused",
      motion_unsupported_mode),
    S("motion-eeprom-lock", "EEPROM is locked while the torque is on",
      "tests/sil/test_motion.py::test_eeprom_is_locked_while_torque_is_on",
      motion_eeprom_locked, moves=True),
]
del S

# SIL tests deliberately not repeated on the board
NOT_REPEATED = {
    "tests/sil/test_boot.py::test_startup_log_has_no_error":
        "reads the firmware log (RTT on the board)",
    "tests/sil/test_protection.py": "the faults (load torque, supply voltage) must be made on "
                                    "the bench",
}


def run_scenarios(bus, scenarios, stop_event, log=print, on_result=lambda result: None):
    """Run the scenarios one after the other; returns the list of ScenarioResult."""
    results = []
    with bus.lock:
        for scenario in scenarios:
            if stop_event.is_set():
                break
            result = ScenarioResult(scenario)
            log(f"{scenario.id}: {scenario.title}")
            ctx = Context(bus, result, stop_event, log)
            t0 = time.monotonic()
            try:
                waited = ctx.restart()
                ctx.info("Fresh start (Reboot)", f"answered after {waited:.2f} s")
                scenario.run(ctx)
            except CheckFailed as exc:
                result.status, result.error = FAIL, str(exc)
            except Stopped:
                result.status, result.error = STOPPED, "stopped by the operator"
            except p.DxlError as exc:
                result.status, result.error = FAIL, f"unexpected status error: {exc}"
            except (TimeoutError, IOError) as exc:
                result.status, result.error = ERROR, f"bus: {exc}"
            finally:
                _torque_off(ctx)
            result.seconds = time.monotonic() - t0
            log(f"  -> {result.status}" + (f": {result.error}" if result.error else ""))
            results.append(result)
            on_result(result)
    return results
