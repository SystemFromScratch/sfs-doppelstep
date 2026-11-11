#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Copy the images of a board build into output/, named with the version from git.

    python3 scripts/export_output.py [--build-dir build]

Run after every board build by .vscode/workspace-menu.sh. The version is
`git describe --tags --dirty`: "0.1.0" on the tag v0.1.0, "0.1.0-3-g1a2b3c4-dirty"
three commits and local changes later. Files, as in the release
(scripts/package_release.py):

    doppelstep-<version>-b_g431b_esc1[-sim-motor]-merged.hex    MCUboot + application:
                                                                 the first flashing (ST-LINK)
    doppelstep-<version>-b_g431b_esc1-<app|sim-motor>.signed.bin update over the bus
                                                                 (MCUboot serial recovery, GUI)
    doppelstep-<version>-b_g431b_esc1-<app|sim-motor>.signed.hex the same as HEX
    doppelstep-<version>-b_g431b_esc1-<app|sim-motor>.elf        debug symbols

output/ is in .gitignore. Only the Python standard library is used.
"""

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARD = "b_g431b_esc1"
SIM_COMPATIBLE = "sfs,sim-power-stage"
EOF_RECORD = ":00000001FF"


def version():
    """Version from the git tag, without the leading v; 'unknown' without git."""
    try:
        out = subprocess.run(["git", "-c", "safe.directory=*", "describe", "--tags", "--dirty",
                              "--always"], cwd=ROOT, capture_output=True, text=True,
                             check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    return out[1:] if out.startswith("v") else out


def merge_hex(sources, target):
    """Intel HEX files one after the other (each sets its own extended address), with
    one end-of-file record. The images must not overlap: MCUboot ends where slot0 starts."""
    with open(target, "w", newline="\n") as out:
        for path in sources:
            with open(path) as f:
                for line in f:
                    line = line.strip()
                    if line and line.upper() != EOF_RECORD:
                        out.write(line + "\n")
        out.write(EOF_RECORD + "\n")


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--build-dir", default=os.path.join(ROOT, "build"))
    p.add_argument("--out", default=os.path.join(ROOT, "output"))
    args = p.parse_args(argv)

    app = os.path.join(args.build_dir, "app", "zephyr")
    boot = os.path.join(args.build_dir, "mcuboot", "zephyr", "zephyr.hex")
    signed_bin = os.path.join(app, "zephyr.signed.bin")
    if not os.path.isfile(signed_bin) or not os.path.isfile(boot):
        print(f"export_output: no sysbuild board build in {args.build_dir}, nothing exported")
        return 0
    with open(os.path.join(app, "zephyr.dts")) as f:
        sim = SIM_COMPATIBLE in f.read()

    variant = "sim-motor" if sim else "app"
    prefix = f"doppelstep-{version()}-{BOARD}"
    os.makedirs(args.out, exist_ok=True)
    files = {
        f"{prefix}{'-sim-motor' if sim else ''}-merged.hex": None,
        f"{prefix}-{variant}.signed.bin": signed_bin,
        f"{prefix}-{variant}.signed.hex": os.path.join(app, "zephyr.signed.hex"),
        f"{prefix}-{variant}.elf": os.path.join(app, "zephyr.elf"),
    }
    for name, source in files.items():
        target = os.path.join(args.out, name)
        if source is None:
            merge_hex([boot, os.path.join(app, "zephyr.signed.hex")], target)
        else:
            shutil.copyfile(source, target)
        print(f"  {os.path.relpath(target, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
