#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Continuous integration in the Docker image, the same on a PC and in GitHub
# Actions (.github/workflows/ci.yml):
#
#   docker compose run --rm zephyr bash scripts/ci.sh
#
#   1. dependencies between the layers (scripts/check-layers.sh)
#   2. west workspace: init / update (scripts/west-init.sh)
#   3. pristine builds without warnings: b_g431b_esc1, b_g431b_esc1 + sim.overlay,
#      native_sim (the test firmware)
#   4. ztest suites with Twister (tests/app, tests/drivers) -> build-ci/twister
#   5. SIL and integration tests (scripts/run-tests.sh) -> build-tests/junit.xml
#   6. SIL motion tests on the Stepper-C fixed-point backend: a floating-point
#      dependency in the drivers or the application fails here, or in step 1
#
# Build logs: build-ci/*.log. Known warning outside this repository (reported
# upstream): -Wmaybe-uninitialized in x430_bus.c of the X430 emulator, host gcc.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# The repository is bind-mounted with another owner than the container user
git config --global --add safe.directory '*'

KNOWN_WARNINGS='dynamixel-x430/src/x430_bus.c:[0-9]+:[0-9]+: warning: .*maybe-uninitialized'

bash scripts/check-layers.sh

bash scripts/west-init.sh

# STEPPER_LAB_SOURCE_DIR: local checkout of Stepper-Lab (tests/README.md)
sim_args=()
[ -n "${STEPPER_LAB_SOURCE_DIR:-}" ] && sim_args=("-DSTEPPER_LAB_SOURCE_DIR=${STEPPER_LAB_SOURCE_DIR}")

mkdir -p build-ci
failed=0

# build <name> <west build arguments...>
build() {
    local name=$1
    shift
    local log="build-ci/${name}.log"
    echo "::group::build ${name}"
    if ! west build -p always "$@" >"${log}" 2>&1; then
        tail -50 "${log}"
        echo "ERROR: build ${name} failed (${log})"
        failed=1
    fi
    local warnings
    warnings=$(grep -E 'warning:' "${log}" | grep -Ev "${KNOWN_WARNINGS}" || true)
    if [ -n "${warnings}" ]; then
        echo "${warnings}"
        echo "ERROR: build ${name} has warnings (${log})"
        failed=1
    fi
    grep -E 'FLASH:|RAM:' "${log}" || true
    echo "::endgroup::"
}

build esc1 -b b_g431b_esc1 -s app -d build-ci/esc1
build esc1-sim -b b_g431b_esc1 -s app -d build-ci/esc1-sim \
    -- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay "${sim_args[@]}"
build native-tests -b native_sim/native/64 -s app -d build-tests \
    -- -DEXTRA_CONF_FILE=sim-test.conf "${sim_args[@]}"

if [ "${failed}" -ne 0 ]; then
    exit 1
fi

echo "::group::ztest suites"
west twister -T tests/app -T tests/drivers -p native_sim/native/64 --integration \
    --outdir build-ci/twister --inline-logs
echo "::endgroup::"

echo "::group::SIL and integration tests"
# run-tests.sh ends with exec, so it runs in a child shell
bash scripts/run-tests.sh
echo "::endgroup::"

echo "::group::SIL motion tests, fixed-point backend"
TESTS_BUILD_DIR=build-tests-fixed TESTS_EXTRA_CMAKE_ARGS=-DCONFIG_SSTEP_MATH_FIXED=y \
    bash scripts/run-tests.sh tests/sil/test_motion.py
echo "::endgroup::"
