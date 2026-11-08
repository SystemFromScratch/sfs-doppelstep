#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# The architecture rules of docs/architecture.md, checked on the C sources: what may
# appear in which layer. Exits with 1 and prints the lines that break a rule.

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

SOURCES=('app/*.c' 'app/*.h' 'drivers/*.c' 'drivers/*.h' 'include/*.h')
failed=0

# check <rule> <regular expression> <pathspecs...>
check() {
    local rule=$1 regex=$2
    shift 2
    local hits
    hits=$(git grep -nP --untracked -e "${regex}" -- "$@")
    if [ -n "${hits}" ]; then
        echo "ERROR: ${rule}"
        echo "${hits}"
        failed=1
    fi
}

check "registers and interrupts of the MCU only in its power stage backend" \
    'stm32_ll|\bDWT\b|\bISR_DIRECT|\bIRQ_DIRECT_CONNECT|\bLL_[A-Z]+_' \
    "${SOURCES[@]}" ':!drivers/power_stage/power_stage_stm32.c'

check "the Stepper-C API only in the motor driver" \
    '<sstep\.h>|\bsstep_(?!motor|plant)|\bSSTEP_(?!MOTOR|PLANT)' \
    "${SOURCES[@]}" ':!drivers/motor/' ':!drivers/power_stage/sim_plant.*'

check "the X430 library only in its driver and in the unit conversions" \
    '#include <(x430|dxl2)' \
    "${SOURCES[@]}" ':!drivers/x430/' ':!include/doppelstep/drivers/x430_servo.h' ':!app/src/joint.c'

check "the motor driver knows no DYNAMIXEL, the X430 driver no motor" \
    'x430_|X430_|DXL2_' 'drivers/motor/*' 'include/doppelstep/drivers/sstep_motor.h'
check "the motor driver knows no DYNAMIXEL, the X430 driver no motor" \
    'sstep_|stepper_|power_stage' 'drivers/x430/*' 'include/doppelstep/drivers/x430_servo.h'

check "the simulation backend never switches the outputs of a real stage" \
    'power_stage_output_enable' 'drivers/power_stage/power_stage_sim.c'

check "no floating point above the power stage backends" \
    '\bfloat\b|\bdouble\b|\bsqrtf?\b|<math\.h>' \
    'app/*.c' 'app/*.h' 'drivers/motor/*' 'drivers/x430/*' 'include/*.h'

if [ "${failed}" -ne 0 ]; then
    exit 1
fi
echo "layers: ok"
