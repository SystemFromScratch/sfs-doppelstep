#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# SIL and integration tests on native_sim (tests/README.md). Runs where west
# runs: the Docker image (Windows host, CI), the Dev Container or a Linux host.
#
#   bash scripts/run-tests.sh                  build, then every test
#   bash scripts/run-tests.sh -k motion        extra arguments go to pytest
#   bash scripts/run-tests.sh tests/sil        only the SIL tests
#
# Environment:
#   TESTS_BUILD_DIR       build directory (default build-tests)
#   TESTS_PRISTINE=1      pristine build
#   SSTEP_SIM_SOURCE_DIR  local checkout of sfs-stepper-sim instead of the west project
#
# Results: <build dir>/junit.xml, firmware logs of every test in
# <build dir>/pytest/<test>/fw.log.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

build_dir=${TESTS_BUILD_DIR:-build-tests}
west_args=(-b native_sim/native/64 -s apps/doppelstep -d "${build_dir}")
[ "${TESTS_PRISTINE:-0}" = 1 ] && west_args+=(-p always)
cmake_args=(-DEXTRA_CONF_FILE=sim-test.conf)
[ -n "${SSTEP_SIM_SOURCE_DIR:-}" ] && cmake_args+=("-DSSTEP_SIM_SOURCE_DIR=${SSTEP_SIM_SOURCE_DIR}")

echo "Building the test firmware (native_sim) into ${build_dir}."
west build "${west_args[@]}" -- "${cmake_args[@]}"

python3 -c 'import pytest, dynamixel_sdk' 2>/dev/null ||
    { echo 'ERROR: pytest / dynamixel_sdk missing: pip install -r tests/requirements.txt' >&2; exit 1; }

rm -rf "${build_dir}/pytest"
exec python3 -m pytest \
    --exe "${build_dir}/zephyr/zephyr.exe" \
    --junitxml "${build_dir}/junit.xml" \
    --basetemp "${build_dir}/pytest" \
    "${@:-tests}"
