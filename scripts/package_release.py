#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check a release and package its firmware (.github/workflows/release.yml).

    python3 scripts/package_release.py --version 0.1.0 --check-only
    python3 scripts/package_release.py --version 0.1.0 [--build-dir build-ci]
                                       [--manual site/Doppelstep.pdf] [--out dist]

Checks (both modes):
  - the version is X.Y.Z and equals app/VERSION (the version MCUboot reads from
    the signed image and the firmware logs at boot)
  - CHANGELOG.md has a section "## [X.Y.Z] - YYYY-MM-DD"
  - every project of west.yml is pinned to a tag vX.Y.Z or a full commit hash,
    so that a release always builds the same code

Packaging: the board images of scripts/ci.sh (build-ci/esc1, build-ci/esc1-sim)
under release names, MCUboot and the application merged into one HEX for the
first flashing, the PDF manual, SHA256SUMS and the release notes (the CHANGELOG
section of the version). The merge needs the Python package intelhex (part of
the Zephyr requirements; on another host: pip install intelhex).
"""

import argparse
import hashlib
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARD = "b_g431b_esc1"

# (build file relative to the build directory, release file suffix)
IMAGES = [
    ("esc1/mcuboot/zephyr/zephyr.hex", "mcuboot.hex"),
    ("esc1/app/zephyr/zephyr.signed.hex", "app.signed.hex"),
    ("esc1/app/zephyr/zephyr.signed.bin", "app.signed.bin"),
    ("esc1/app/zephyr/zephyr.elf", "app.elf"),
    ("esc1-sim/app/zephyr/zephyr.signed.hex", "sim-motor.signed.hex"),
    ("esc1-sim/app/zephyr/zephyr.signed.bin", "sim-motor.signed.bin"),
    ("esc1-sim/app/zephyr/zephyr.elf", "sim-motor.elf"),
]
MERGED = ("esc1/mcuboot/zephyr/zephyr.hex", "esc1/app/zephyr/zephyr.signed.hex", "merged.hex")


def fail(message):
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(1)


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


def app_version():
    fields = dict(re.findall(r"^\s*(\w+)\s*=\s*(\S*)\s*$", read("app/VERSION"), re.MULTILINE))
    try:
        return "{}.{}.{}".format(fields["VERSION_MAJOR"], fields["VERSION_MINOR"],
                                 fields["PATCHLEVEL"])
    except KeyError as missing:
        fail(f"app/VERSION has no {missing}")


def changelog_section(version):
    """Text of the CHANGELOG section of the version, without its heading."""
    lines = read("CHANGELOG.md").splitlines()
    heading = f"## [{version}]"
    start = next((i for i, line in enumerate(lines) if line.startswith(heading)), None)
    if start is None:
        fail(f"CHANGELOG.md has no section '{heading} - YYYY-MM-DD'")
    if not re.fullmatch(re.escape(heading) + r" - \d{4}-\d{2}-\d{2}", lines[start].strip()):
        fail(f"CHANGELOG.md: '{lines[start]}' is not '{heading} - YYYY-MM-DD'")
    body = []
    for line in lines[start + 1:]:
        if line.startswith("## [") or re.match(r"^\[[^\]]+\]: ", line):
            break
        body.append(line)
    text = "\n".join(body).strip()
    if not text:
        fail(f"CHANGELOG.md: the section {heading} is empty")
    return text


def unpinned_projects():
    """(name, revision) of every west project that follows a branch."""
    found, name = [], None
    for line in read("west.yml").splitlines():
        match = re.match(r"^\s*-\s*name:\s*(\S+)", line)
        if match:
            name = match.group(1)
            continue
        match = re.match(r"^\s*revision:\s*(\S+)", line)
        if match:
            revision = match.group(1).strip("\"'")
            if not re.fullmatch(r"v\d+\.\d+\.\d+|[0-9a-f]{40}", revision):
                found.append((name, revision))
    return found


def check(version):
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        fail(f"'{version}' is not a version X.Y.Z (the tag is vX.Y.Z)")
    if app_version() != version:
        fail(f"app/VERSION is {app_version()}, the tag is v{version}")
    notes = changelog_section(version)
    unpinned = unpinned_projects()
    if unpinned:
        fail("west.yml: not pinned to a tag or a commit: "
             + ", ".join(f"{name} ({revision})" for name, revision in unpinned))
    print(f"release {version}: app/VERSION, CHANGELOG.md and west.yml ok")
    return notes


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def package(version, build_dir, manual, out, notes):
    prefix = f"doppelstep-{version}-{BOARD}"
    os.makedirs(out, exist_ok=True)
    written = []

    for source, suffix in IMAGES:
        path = os.path.join(build_dir, source)
        if not os.path.isfile(path):
            fail(f"{path} is missing: run scripts/ci.sh first")
        target = os.path.join(out, f"{prefix}-{suffix}")
        shutil.copyfile(path, target)
        written.append(target)

    try:
        from intelhex import IntelHex
    except ImportError:
        fail("the Python package intelhex is missing: pip install intelhex")
    merged = IntelHex()
    for source in MERGED[:2]:
        merged.merge(IntelHex(os.path.join(build_dir, source)), overlap="error")
    target = os.path.join(out, f"{prefix}-{MERGED[2]}")
    merged.write_hex_file(target)
    written.append(target)

    if manual:
        if not os.path.isfile(manual):
            fail(f"{manual} is missing: python tools/docs/build_manual.py")
        target = os.path.join(out, f"doppelstep-{version}-manual.pdf")
        shutil.copyfile(manual, target)
        written.append(target)

    with open(os.path.join(out, "SHA256SUMS"), "w", encoding="utf-8", newline="\n") as f:
        for path in sorted(written):
            f.write(f"{sha256(path)}  {os.path.basename(path)}\n")

    with open(os.path.join(out, "release-notes.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(notes + "\n")

    for path in sorted(written):
        print(f"  {os.path.relpath(path, ROOT)}")
    print(f"  {os.path.relpath(os.path.join(out, 'SHA256SUMS'), ROOT)}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--version", required=True, help="X.Y.Z, the tag without the v")
    parser.add_argument("--check-only", action="store_true", help="only the checks")
    parser.add_argument("--build-dir", default=os.path.join(ROOT, "build-ci"))
    parser.add_argument("--manual", help="PDF manual to include")
    parser.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = parser.parse_args(argv)

    notes = check(args.version)
    if not args.check_only:
        package(args.version, args.build_dir, args.manual, args.out, notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
