#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Linux actions of the VS Code tasks (.vscode/tasks.json): inside the Dev
# Container, or on a Linux host. The Windows host counterpart is
# .vscode/host-menu.ps1.
#
#   bash .vscode/workspace-menu.sh "<action>"
#
# Zephyr actions need west. On a Linux host without west they run in the
# Docker image through compose.yaml (service zephyr, or zephyr-usb for the
# actions that use the debug probe).
#
# Python actions (GUI, documentation) use a virtual environment with
# requirements.txt: .venv, or .venv-linux when .venv is a Windows environment
# in the folder bind-mounted from a Windows host.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

APP=apps/doppelstep
BOARD_TARGET=b_g431b_esc1
BUILD_DIR=build
GUI=tools/gui/doppelstep_gui.py
PDF=site/Doppelstep.pdf

die() {
    echo "ERROR: $*" >&2
    exit 1
}

# --- Zephyr -------------------------------------------------------------------

# Runs this script with the same action in the Docker image when west is not
# installed on a Linux host. $1: compose service.
zephyr_in_docker() {
    local service=$1
    shift
    if [ -f /.dockerenv ] || command -v west >/dev/null 2>&1; then
        return 0
    fi
    command -v docker >/dev/null 2>&1 ||
        die 'west is not installed and Docker is not available. Open the folder in the Dev Container or install the Zephyr tools.'
    echo "west not found: running \"$*\" in the Docker image (compose service ${service})."
    if [ "${service}" = zephyr-usb ]; then
        exec docker compose --profile usb run --rm "${service}" bash .vscode/workspace-menu.sh "$@"
    fi
    exec docker compose run --rm "${service}" bash .vscode/workspace-menu.sh "$@"
}

# West topdir is the parent of this repository (scripts/west-init.sh).
ensure_workspace() {
    if ! west topdir >/dev/null 2>&1 || ! west list zephyr >/dev/null 2>&1; then
        echo 'West workspace not ready: initializing and updating it.'
        bash scripts/west-init.sh
    fi
}

# build_firmware [west build options...]
build_firmware() {
    ensure_workspace
    echo "Building ${APP} for ${BOARD_TARGET} into ${BUILD_DIR}."
    west build "$@" -b "${BOARD_TARGET}" -s "${APP}" -d "${BUILD_DIR}"
}

require_build() {
    [ -f "${BUILD_DIR}/zephyr/zephyr.elf" ] || build_firmware
}

# --- Python tools -------------------------------------------------------------

venv_dir() {
    if [ -d .venv/Scripts ] && [ ! -x .venv/bin/python ]; then
        echo .venv-linux
    else
        echo .venv
    fi
}

# ensure_venv "<python imports that must work>"
ensure_venv() {
    local venv py
    venv=$(venv_dir)
    py=${venv}/bin/python
    if [ -x "${py}" ] && "${py}" -c "import $1" >/dev/null 2>&1; then
        return 0
    fi
    command -v python3 >/dev/null 2>&1 || die 'python3 not found (Debian/Ubuntu: apt install python3 python3-venv).'
    if [ ! -x "${py}" ]; then
        echo "Creating virtual environment in ${venv}."
        python3 -m venv "${venv}" || die 'python3 -m venv failed (Debian/Ubuntu: apt install python3-venv).'
    fi
    echo 'Installing requirements.txt.'
    "${py}" -m pip install --upgrade pip
    "${py}" -m pip install -r requirements.txt
    echo "${venv} is ready: ${py}"
}

# Chromium of Playwright for the PDF renderer, once per machine or container.
ensure_chromium() {
    local py cache
    py=$(venv_dir)/bin/python
    cache=${PLAYWRIGHT_BROWSERS_PATH:-${HOME}/.cache/ms-playwright}
    if compgen -G "${cache}/chromium-*" >/dev/null; then
        return 0
    fi
    echo 'Installing Chromium for the PDF renderer (Playwright).'
    if [ "$(id -u)" = 0 ]; then
        "${py}" -m playwright install --with-deps chromium
    else
        "${py}" -m playwright install chromium
    fi
}

run_gui() {
    [ -f "${GUI}" ] || die "GUI not present yet: expected ${GUI} (see tools/gui/README.md)."
    ensure_venv 'serial, tkinter'
    "$(venv_dir)/bin/python" -c 'import tkinter' 2>/dev/null ||
        die 'Tkinter is missing (Debian/Ubuntu: apt install python3-tk, then delete the venv).'
    [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] ||
        die 'No display: run the GUI on the host (Windows task or a Linux desktop), not in the container.'
    echo 'Starting the Doppelstep GUI. Close the window to finish the task.'
    "$(venv_dir)/bin/python" "${GUI}" "$@"
}

build_pdf() {
    ensure_venv 'mkdocs, material, playwright, pypdf'
    ensure_chromium
    "$(venv_dir)/bin/python" tools/docs/build_pdf.py
    echo "PDF manual: ${PDF}"
}

serve_docs() {
    ensure_venv 'mkdocs, material'
    echo 'Documentation preview on http://127.0.0.1:8000 (Ctrl+C stops it).'
    NO_MKDOCS_2_WARNING=1 "$(venv_dir)/bin/python" -m mkdocs serve -a 127.0.0.1:8000
}

# --- Actions ------------------------------------------------------------------

action=${1:-}

case "${action}" in
    'Initialize + update')
        zephyr_in_docker zephyr "${action}"
        bash scripts/west-init.sh
        ;;
    'Build firmware')
        zephyr_in_docker zephyr "${action}"
        build_firmware
        ;;
    'Pristine build')
        zephyr_in_docker zephyr "${action}"
        build_firmware -p always
        ;;
    'Menuconfig')
        zephyr_in_docker zephyr "${action}"
        require_build
        west build -d "${BUILD_DIR}" -t menuconfig
        ;;
    'Flash (OpenOCD, ST-LINK)')
        zephyr_in_docker zephyr-usb "${action}"
        require_build
        west flash -d "${BUILD_DIR}" -r openocd
        ;;
    'Flash (J-Link)')
        zephyr_in_docker zephyr-usb "${action}"
        require_build
        west flash -d "${BUILD_DIR}" -r jlink
        ;;
    'Debug server (OpenOCD, ST-LINK)')
        zephyr_in_docker zephyr-usb "${action}"
        require_build
        west debugserver -d "${BUILD_DIR}" -r openocd
        ;;
    'Debug server (J-Link)')
        zephyr_in_docker zephyr-usb "${action}"
        require_build
        west debugserver -d "${BUILD_DIR}" -r jlink
        ;;
    'RTT log (OpenOCD)')
        zephyr_in_docker zephyr-usb "${action}"
        require_build
        west rtt -d "${BUILD_DIR}" -r openocd
        ;;
    'RTT log (J-Link)')
        zephyr_in_docker zephyr-usb "${action}"
        require_build
        west rtt -d "${BUILD_DIR}" -r jlink
        ;;
    'Ccache stats')
        zephyr_in_docker zephyr "${action}"
        ccache --show-stats
        ;;
    'Ccache clear')
        zephyr_in_docker zephyr "${action}"
        ccache --clear
        ;;
    'Clean')
        echo "Removing ${BUILD_DIR}/."
        rm -rf "${BUILD_DIR}"
        ;;
    'Create .venv')
        ensure_venv 'serial, mkdocs, material, playwright, pypdf'
        ;;
    'Run GUI')
        run_gui
        ;;
    'Run GUI (simulator)')
        run_gui --sim
        ;;
    'Serve documentation')
        serve_docs
        ;;
    'Build PDF documentation')
        build_pdf
        ;;
    *)
        echo "Unknown workspace action: ${action}" >&2
        exit 2
        ;;
esac
