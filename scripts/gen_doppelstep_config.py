#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate doppelstep_config.h: the factory Control Table and the driver data of a build.

The devicetree of the build is the only source. This header shows, for one build, every
factory value of the EEPROM area of the emulated XM430-W350 and where it comes from, and
the driver data they are derived from (bus voltage limits, currents, velocity). The
firmware applies the DOPPELSTEP_FACTORY_* values that differ from the XM430
(app/src/joint_bridge.c); edit the devicetree, not this file.

  gen_doppelstep_config.py --edt-pickle build/app/zephyr/edt.pickle \
      --x430-dir modules/lib/dynamixel-x430 --overrides app/x430_overrides.yaml \
      --output build/app/doppelstep/doppelstep_config.h

Called by app/CMakeLists.txt at CMake configure time (the devicetree changes only then).
"""

import argparse
import os
import pathlib
import pickle
import re
import sys

# Units of the XM430-W350 Control Table, as in app/src/joint.c
CURRENT_UNIT = (269, 100)     # 2.69 mA
VELOCITY_UNIT = (229, 60000)  # 0.229 rev/min, in rev/s: 229 / 60000
VELOCITY_LIMIT_MAX = 1023     # largest Velocity Limit of the X430 register


def div_round(num, den):
    """Round half away from zero, as div_round() of app/src/joint.c."""
    q, r = divmod(abs(num), den)
    if 2 * r >= den:
        q += 1
    return q if num >= 0 else -q


def largest_not_above(limit, to_dxl, from_dxl):
    """Largest register value whose physical value does not exceed `limit`."""
    value = to_dxl(limit)
    while value > 0 and from_dxl(value) > limit:
        value -= 1
    return value


def prop(node, name):
    if name not in node.props:
        sys.exit(f"gen_doppelstep_config.py: {node.path} has no property {name}")
    return node.props[name].val


def chosen(edt, name):
    node = edt.chosen_nodes.get(name)
    if node is None:
        sys.exit(f"gen_doppelstep_config.py: no chosen node {name}")
    return node


def factory_values(edt):
    """{register name: (value, source)} for the values that come from the devicetree."""
    servo = chosen(edt, "sfs,servo")
    motor = chosen(edt, "sfs,motor")
    stage = prop(motor, "power-stage")

    rated_ma = prop(motor, "rated-current-milliamp")
    current = largest_not_above(
        rated_ma,
        lambda ma: div_round(ma * CURRENT_UNIT[1], CURRENT_UNIT[0]),
        lambda c: div_round(c * CURRENT_UNIT[0], CURRENT_UNIT[1]))

    # Velocity at the joint: microsteps per joint revolution = steps x microsteps x gear
    usteps_num = (prop(motor, "full-steps-per-rev") * prop(motor, "microsteps")
                  * prop(servo, "gear-motor-revs"))
    usteps_den = prop(servo, "gear-joint-revs")
    v_max = prop(motor, "max-velocity")
    velocity = largest_not_above(
        v_max,
        lambda v: div_round(v * VELOCITY_UNIT[1] * usteps_den, VELOCITY_UNIT[0] * usteps_num),
        lambda d: div_round(d * VELOCITY_UNIT[0] * usteps_num, VELOCITY_UNIT[1] * usteps_den))

    vbus_max = prop(motor, "vbus-max-mv")
    vbus_min = prop(motor, "vbus-min-mv")
    return {
        "ID": (prop(servo, "factory-id"), "factory-id of the servo node"),
        "Baud Rate": (prop(servo, "factory-baud-code"), "factory-baud-code of the servo node"),
        "Temperature Limit": (prop(stage, "temperature-limit-celsius"),
                              "temperature-limit-celsius of the power stage node [degC]"),
        "Max Voltage Limit": (vbus_max // 100,
                              f"vbus-max-mv {vbus_max} of the motor node [0.1 V], rounded down"),
        "Min Voltage Limit": (-(-vbus_min // 100),
                              f"vbus-min-mv {vbus_min} of the motor node [0.1 V], rounded up"),
        "Current Limit": (current, f"rated-current-milliamp {rated_ma} of the motor node "
                                   "[2.69 mA], rounded down"),
        "Velocity Limit": (min(velocity, VELOCITY_LIMIT_MAX),
                           f"max-velocity {v_max} microsteps/s of the motor node at the joint "
                           f"[0.229 rev/min] = {velocity}, at most {VELOCITY_LIMIT_MAX}"),
    }


def driver_data(edt):
    """(macro, value, comment) of the driver data the factory values come from."""
    servo = chosen(edt, "sfs,servo")
    motor = chosen(edt, "sfs,motor")
    stage = prop(motor, "power-stage")
    rows = [
        ("VBUS_NOMINAL_MV", prop(motor, "vbus-nominal-mv"), "nominal bus voltage [mV]"),
        ("VBUS_MIN_MV", prop(motor, "vbus-min-mv"), "undervoltage trip [mV]"),
        ("VBUS_MAX_MV", prop(motor, "vbus-max-mv"), "overvoltage trip [mV]"),
        ("RATED_CURRENT_MA", prop(motor, "rated-current-milliamp"),
         "rated phase current of the motor [mA]: highest run current over the bus"),
        ("RUN_CURRENT_MA", prop(motor, "run-current-milliamp"),
         "run current at startup and after Factory Reset [mA]"),
        ("HOLD_CURRENT_MA", prop(motor, "hold-current-milliamp"), "hold current [mA]"),
        ("TRIP_CURRENT_MA", prop(motor, "trip-current-milliamp"), "overcurrent trip [mA]"),
        ("MAX_VELOCITY", prop(motor, "max-velocity"), "largest velocity [microsteps/s]"),
        ("MAX_ACCELERATION", prop(motor, "max-acceleration"),
         "largest acceleration [microsteps/s^2]"),
        ("FULL_STEPS_PER_REV", prop(motor, "full-steps-per-rev"), "full steps per revolution"),
        ("MICROSTEPS", prop(motor, "microsteps"), "microsteps per full step"),
        ("GEAR_MOTOR_REVS", prop(servo, "gear-motor-revs"), "motor revolutions ..."),
        ("GEAR_JOINT_REVS", prop(servo, "gear-joint-revs"), "... per joint revolutions"),
        ("TEMPERATURE_LIMIT_C", prop(stage, "temperature-limit-celsius"),
         "power stage temperature limit [degC]"),
    ]
    # Optional properties: the simulated power stage (sim.overlay) has no PWM timer
    # and no hardware trips
    for name, macro, comment in (("pwm-frequency", "PWM_FREQUENCY_HZ", "PWM frequency [Hz]"),
                                 ("hw-trip-current-milliamp", "HW_TRIP_CURRENT_MA",
                                  "hardware overcurrent trip [mA]"),
                                 ("hw-trip-vbus-mv", "HW_TRIP_VBUS_MV",
                                  "hardware overvoltage trip [mV]"),
                                 ("dead-time-ns", "DEAD_TIME_NS", "dead time [ns]")):
        if name in stage.props:
            rows.append((macro, stage.props[name].val, comment))
    return rows


def macro_name(register):
    return re.sub(r"[^A-Z0-9]+", "_", register.upper()).strip("_")


def render(model, factory, data, sources):
    lines = [
        "/*",
        " * SPDX-License-Identifier: Apache-2.0",
        " *",
        " * Generated by scripts/gen_doppelstep_config.py from the devicetree of this build:",
        " * do not edit. Change the devicetree (app/boards/<board>.overlay) instead.",
        f" * Inputs: {', '.join(sources)}",
        " */",
        "",
        "#ifndef DOPPELSTEP_CONFIG_H_",
        "#define DOPPELSTEP_CONFIG_H_",
        "",
        "/* --- Driver data (devicetree) ------------------------------------------------ */",
        "",
    ]
    for name, value, comment in data:
        lines.append(f"/** {comment} */")
        lines.append(f"#define DOPPELSTEP_{name} {value}")
    lines += [
        "",
        f"/* --- Factory EEPROM area of the {model.name} (addresses 0..63) ----------- */",
        "/* DOPPELSTEP_FACTORY_<REGISTER> = value after Factory Reset and at the first start.",
        " * DOPPELSTEP_FACTORY_<REGISTER>_FROM_DT = 1: the value comes from the devicetree and",
        " * the firmware writes it; 0: the value of the XM430 model (initial value of the",
        " * emulator). DOPPELSTEP_RANGE_<REGISTER>_MIN/_MAX: accepted range of a write.",
        " */",
        "",
    ]
    lines += ["/* Read-only identity, not factory values: Model Number (0), Model Information (2)",
              " * = firmware version (doppelstep_version.h), Firmware Version (6) = 52 (XM430).",
              " */", ""]
    for reg in model.registers:
        if reg.area != "eeprom" or reg.access == "r":
            continue
        name = macro_name(reg.name)
        if reg.name in factory:
            value, source = factory[reg.name]
            from_dt = 1
        else:
            value, source, from_dt = reg.initial, "XM430 model", 0
        if reg.check == "range" and not reg.minimum <= value <= reg.maximum:
            sys.exit(f"gen_doppelstep_config.py: {reg.name} {value} ({source}) outside "
                     f"{reg.minimum}..{reg.maximum} of the Control Table")
        lines.append(f"/** {reg.name} ({reg.address}): {source} */")
        lines.append(f"#define DOPPELSTEP_FACTORY_{name} {value}")
        lines.append(f"#define DOPPELSTEP_FACTORY_{name}_FROM_DT {from_dt}")
        if reg.check == "range":
            lines.append(f"#define DOPPELSTEP_RANGE_{name}_MIN {reg.minimum}")
            lines.append(f"#define DOPPELSTEP_RANGE_{name}_MAX {reg.maximum}")
    missing = set(factory) - {r.name for r in model.registers if r.area == "eeprom"}
    if missing:
        sys.exit(f"gen_doppelstep_config.py: not EEPROM registers of the model: {sorted(missing)}")
    lines += ["", "#endif /* DOPPELSTEP_CONFIG_H_ */", ""]
    return "\n".join(lines)


def write_if_changed(path, text):
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--edt-pickle", required=True)
    parser.add_argument("--x430-dir", required=True, help="DYNAMIXEL-X430 emulator source tree")
    parser.add_argument("--model", default="xm430_w350")
    parser.add_argument("--overrides", default="", help="model overrides (YAML), as the build")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    zephyr_base = os.environ.get("ZEPHYR_BASE", "")
    sys.path.insert(0, os.path.join(zephyr_base, "scripts", "dts", "python-devicetree", "src"))
    sys.path.insert(0, os.path.join(args.x430_dir, "tools"))
    import x430_gen_model as gen  # noqa: E402  (the emulator's own model resolver)

    with open(args.edt_pickle, "rb") as f:
        edt = pickle.load(f)
    overrides = gen.load_overrides(pathlib.Path(args.overrides)) if args.overrides else {}
    model = gen.resolve(pathlib.Path(args.x430_dir) / "models", args.model,
                        extra_overrides=overrides)

    sources = ["devicetree", f"model {args.model}"]
    if args.overrides:
        sources.append(os.path.basename(args.overrides))
    write_if_changed(args.output, render(model, factory_values(edt), driver_data(edt), sources))
    return 0


if __name__ == "__main__":
    sys.exit(main())
