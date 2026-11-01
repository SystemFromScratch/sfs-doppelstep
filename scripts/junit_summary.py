#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Markdown summary of a pytest JUnit report (GitHub Actions job summary).

  python3 scripts/junit_summary.py build-tests/junit.xml >> "$GITHUB_STEP_SUMMARY"
"""

import os
import sys
import xml.etree.ElementTree as ET


def main(path):
    sys.stdout.reconfigure(encoding="utf-8")          # the job summary is UTF-8
    if not os.path.exists(path):
        print(f"### Tests\n\nNo report ({path}): the build failed before the tests.")
        return 0
    rows = []
    totals = {"passed": 0, "failed": 0, "skipped": 0, "xfail": 0}
    for case in ET.parse(path).getroot().iter("testcase"):
        name = f"{case.get('classname')}::{case.get('name')}"
        if case.find("failure") is not None or case.find("error") is not None:
            state = "failed"
        elif (skip := case.find("skipped")) is not None:
            state = "xfail" if "xfail" in (skip.get("type") or "") + (skip.get("message") or "") \
                else "skipped"
        else:
            state = "passed"
        totals[state] += 1
        rows.append((state, name, float(case.get("time") or 0)))

    icon = {"passed": "✅", "failed": "❌", "skipped": "⏭️", "xfail": "⚠️ expected failure"}
    print("### SIL and integration tests\n")
    print(", ".join(f"{n} {k}" for k, n in totals.items() if n) + "\n")
    print("| | Test | s |\n|---|---|---|")
    for state, name, seconds in rows:
        print(f"| {icon[state]} | `{name}` | {seconds:.1f} |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build-tests/junit.xml"))
