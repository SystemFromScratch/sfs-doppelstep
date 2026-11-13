#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Continuous integration in the Docker image, the same on a PC and in GitHub
# Actions (.github/workflows/ci.yml):
#
#   docker compose run --rm zephyr bash scripts/ci.sh
#
#   1. dependencies between the layers (scripts/check-layers.sh), host-only tests
#      of the GUI (tools/gui/tests: protocol, scenarios, update sequence)
#   2. west workspace: init / update (scripts/west-init.sh)
#   3. pristine builds without warnings: b_g431b_esc1 and b_g431b_esc1 + sim.overlay
#      with sysbuild (MCUboot + signed application, then build_checks),
#      native_sim (the test firmware)
#   4. ztest suites with Twister (tests/app, tests/drivers) -> build-ci/twister
#   5. SIL and integration tests (scripts/run-tests.sh) -> build-tests/junit.xml
#   6. SIL motion tests on the Stepper-C fixed-point backend: a floating-point
#      dependency in the drivers or the application fails here, or in step 1
#
# Build logs: build-ci/*.log. Known warnings outside this repository:
#   - -Wmaybe-uninitialized in x430_bus.c of the X430 emulator, host gcc
#     (reported upstream)
#   - MCUboot (sysbuild): the RSA check through TF-PSA-Crypto selects
#     TEST_RANDOM_GENERATOR when there is no entropy driver; verifying a
#     signature needs no random numbers
#   - application (sysbuild): MCUboot sets MCUBOOT_UPDATE_FOOTER_SIZE in every
#     image it boots, also without the MCUmgr image manager

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# The repository is bind-mounted with another owner than the container user
git config --global --add safe.directory '*'

KNOWN_WARNINGS='dynamixel-x430/src/x430_bus.c:[0-9]+:[0-9]+: warning: .*maybe-uninitialized'
KNOWN_WARNINGS+='|^warning: Not secure symbol TEST_RANDOM_GENERATOR is enabled\.$'
KNOWN_WARNINGS+='|^warning: MCUBOOT_UPDATE_FOOTER_SIZE \(defined at subsys/dfu/Kconfig:[0-9]+\) was assigned'

bash scripts/check-layers.sh

echo "::group::GUI tests (host-only)"
python3 -m pytest tools/gui/tests -q -p no:cacheprovider
echo "::endgroup::"

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

# check <description> <command...>: the command must succeed
check() {
    local what=$1
    shift
    if ! "$@" >/dev/null 2>&1; then
        echo "ERROR: ${what}"
        failed=1
    fi
}

# What the builds must contain (docs/firmware-update.md)
build_checks() {
    local esc1=build-ci/esc1 sim=build-ci/esc1-sim
    echo "::group::build checks"
    check "MCUboot image" test -f "${esc1}/mcuboot/zephyr/zephyr.hex"
    check "signed application image" test -f "${esc1}/app/zephyr/zephyr.signed.bin"
    check "MCUboot linked into boot_partition" \
        grep -q 'zephyr,code-partition = &boot_partition;' "${esc1}/mcuboot/zephyr/zephyr.dts"
    check "MCUboot: USART2 at 1 Mbps" \
        grep -q 'current-speed = < 0xf4240 >' "${esc1}/mcuboot/zephyr/zephyr.dts"
    check "MCUboot: 500 ms serial recovery window" \
        grep -q '^CONFIG_BOOT_SERIAL_WAIT_FOR_DFU_TIMEOUT=500$' "${esc1}/mcuboot/zephyr/.config"
    check "MCUboot: serial recovery without a valid image" \
        grep -q '^CONFIG_BOOT_SERIAL_NO_APPLICATION=y$' "${esc1}/mcuboot/zephyr/.config"
    check "MCUboot: no console" \
        grep -q '^# CONFIG_CONSOLE is not set$' "${esc1}/mcuboot/zephyr/.config"
    check "MCUboot: no UART console" \
        bash -c "! grep -q '^CONFIG_UART_CONSOLE=y$' '${esc1}/mcuboot/zephyr/.config'"
    check "storage partition at 0x1f000, 4 KiB" \
        grep -q 'reg = < 0x1f000 0x1000 >' "${esc1}/app/zephyr/zephyr.dts"
    check "sim.overlay in the application" \
        grep -q 'sfs,sim-power-stage' "${sim}/app/zephyr/zephyr.dts"
    check "sim.overlay not in MCUboot" \
        bash -c "test -f '${sim}/mcuboot/zephyr/zephyr.dts' &&
                 ! grep -q 'sfs,sim-power-stage' '${sim}/mcuboot/zephyr/zephyr.dts'"
    check "DYNAMIXEL Reboot resets the MCU on the board" \
        grep -q '^CONFIG_SFS_DXL_REBOOT_RESETS_MCU=y$' "${esc1}/app/zephyr/.config"
    check "DYNAMIXEL Reboot stays emulated on native_sim" \
        grep -q '^# CONFIG_SFS_DXL_REBOOT_RESETS_MCU is not set$' build-tests/zephyr/.config
    check "MCUViewer variables off in the release build" \
        grep -q '^# CONFIG_SSTEP_MOTOR_MCUVIEWER is not set$' "${esc1}/app/zephyr/.config"
    check "MCUViewer variables in the bench build" \
        grep -q '^CONFIG_SSTEP_MOTOR_MCUVIEWER=y$' build-ci/esc1-mcuviewer/app/zephyr/.config
    echo "::endgroup::"
}

build esc1 --sysbuild -b b_g431b_esc1 -s app -d build-ci/esc1
build esc1-sim --sysbuild -b b_g431b_esc1 -s app -d build-ci/esc1-sim \
    -- -DEXTRA_DTC_OVERLAY_FILE=sim.overlay "${sim_args[@]}"
build esc1-mcuviewer --sysbuild -b b_g431b_esc1 -s app -d build-ci/esc1-mcuviewer \
    -- -DEXTRA_CONF_FILE=mcuviewer.conf
build native-tests -b native_sim/native/64 -s app -d build-tests \
    -- -DEXTRA_CONF_FILE=sim-test.conf "${sim_args[@]}"
build_checks

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
