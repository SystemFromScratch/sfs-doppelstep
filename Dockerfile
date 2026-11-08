# syntax=docker/dockerfile:1.7
#
# Build and debug environment of Doppelstep (Zephyr 4.5, B-G431B-ESC1).
# Same base and tool versions as Zephyr-SmartFusion2-workspace, plus the
# Python packages of the libraries and the debug runners.

# Zephyr 4.5 needs Python 3.12 or later; Debian bookworm ships 3.11.
FROM --platform=linux/amd64 python:3.12.15-slim-bookworm@sha256:2ed6491b93cd49272ee6de2b5a38440c3448360322c089fc23e370722d74179d

ARG DEBIAN_FRONTEND=noninteractive

# ---------------------------------------------------------------------------
# Versions
# ---------------------------------------------------------------------------

ARG WEST_VERSION=1.5.0
ARG ZEPHYR_SDK_VERSION=1.0.1
ARG JSONSCHEMA_VERSION=4.26.0
ARG CMAKE_VERSION=3.30.5
ARG PYELFTOOLS_VERSION=0.33
# DYNAMIXEL-X430 model generator (tools/x430_gen_model.py); version pinned in
# zephyr/scripts/requirements-actions.txt of Zephyr 4.5.0
ARG PYYAML_VERSION=6.0.3
# `west flash|debug|debugserver|rtt -r jlink` (runners/jlink.py)
ARG PYLINK_SQUARE_VERSION=2.0.1
# SIL and integration tests (tests/requirements.txt: keep both in step)
ARG PYTEST_VERSION=9.0.2
ARG PYTEST_TIMEOUT_VERSION=2.4.0
ARG PYSERIAL_VERSION=3.5
ARG DYNAMIXEL_SDK_VERSION=4.1.0
# Twister (ztest suites in tests/app and tests/drivers)
ARG ANYTREE_VERSION=2.13.0
ARG JUNITPARSER_VERSION=5.0.3
ARG NATSORT_VERSION=8.4.0
ARG PLY_VERSION=3.11
ARG TABULATE_VERSION=0.10.0

# sha256.sum of the SDK release
ARG ZEPHYR_SDK_MINIMAL_SHA256=ca9bc0ff66fafca1dac9d592a36d953cf16d096a9d09b1c0357f021cf9f6a7eb
ARG ZEPHYR_ARM_TOOLCHAIN_SHA256=21b85981cb5a1818d9bc53d82af80f208946ec038b982ff1907287572ed3a634

ENV \
    ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk \
    ZEPHYR_TOOLCHAIN_VARIANT=zephyr \
    BOARD=b_g431b_esc1 \
    VIRTUAL_ENV=/opt/venv \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PIP_NO_CACHE_DIR=1 \
    CCACHE_DIR=/workdir/.ccache \
    PATH=/opt/venv/bin:/opt/zephyr-sdk/gnu/arm-zephyr-eabi/bin:/opt/SEGGER/JLink:${PATH}

# ---------------------------------------------------------------------------
# OS dependencies
# ---------------------------------------------------------------------------

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        ccache \
        device-tree-compiler \
        file \
        g++ \
        gcc \
        git \
        gperf \
        make \
        ninja-build \
        usbutils \
        wget \
        xz-utils \
    && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------------------
# Python environment
#
# west, plus the packages the Zephyr 4.5 build needs (manifest and module
# validation with jsonschema, ELF post-processing with pyelftools), PyYAML for
# the X430 Control Table generator and pylink-square for the J-Link runner.
# CMake comes from pip because Zephyr follows recent CMake releases. pytest,
# pyserial and the DYNAMIXEL SDK run the SIL and integration tests (tests/);
# anytree, junitparser, natsort, ply and tabulate are what Twister needs for the
# ztest suites on native_sim.
# ---------------------------------------------------------------------------

RUN python3 -m venv "${VIRTUAL_ENV}" \
    && "${VIRTUAL_ENV}/bin/pip" install \
        --no-cache-dir \
        "west==${WEST_VERSION}" \
        "jsonschema==${JSONSCHEMA_VERSION}" \
        "cmake==${CMAKE_VERSION}" \
        "pyelftools==${PYELFTOOLS_VERSION}" \
        "PyYAML==${PYYAML_VERSION}" \
        "pylink-square==${PYLINK_SQUARE_VERSION}" \
        "pytest==${PYTEST_VERSION}" \
        "pytest-timeout==${PYTEST_TIMEOUT_VERSION}" \
        "pyserial==${PYSERIAL_VERSION}" \
        "dynamixel-sdk==${DYNAMIXEL_SDK_VERSION}" \
        "anytree==${ANYTREE_VERSION}" \
        "junitparser==${JUNITPARSER_VERSION}" \
        "natsort==${NATSORT_VERSION}" \
        "ply==${PLY_VERSION}" \
        "tabulate==${TABULATE_VERSION}"

# ---------------------------------------------------------------------------
# Zephyr SDK - minimal bundle plus ONLY the ARM Cortex-M toolchain
#
# From SDK 1.0 the GNU toolchains are separate archives named
# toolchain_gnu_<host>_<target> and live under <sdk>/gnu/.
# setup.sh -h installs the host tools, among them OpenOCD with Zephyr RTOS
# awareness (used by the openocd runner and by Cortex-Debug).
# ---------------------------------------------------------------------------

RUN set -eux; \
    base="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${ZEPHYR_SDK_VERSION}"; \
    mkdir -p "${ZEPHYR_SDK_INSTALL_DIR}/gnu"; \
    wget -q "${base}/zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-x86_64_minimal.tar.xz" \
        -O /tmp/zephyr-sdk-minimal.tar.xz; \
    echo "${ZEPHYR_SDK_MINIMAL_SHA256}  /tmp/zephyr-sdk-minimal.tar.xz" | sha256sum -c -; \
    tar -xJf /tmp/zephyr-sdk-minimal.tar.xz --strip-components=1 -C "${ZEPHYR_SDK_INSTALL_DIR}"; \
    wget -q "${base}/toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz" \
        -O /tmp/zephyr-arm-toolchain.tar.xz; \
    echo "${ZEPHYR_ARM_TOOLCHAIN_SHA256}  /tmp/zephyr-arm-toolchain.tar.xz" | sha256sum -c -; \
    tar -xJf /tmp/zephyr-arm-toolchain.tar.xz -C "${ZEPHYR_SDK_INSTALL_DIR}/gnu"; \
    rm -f /tmp/zephyr-sdk-minimal.tar.xz /tmp/zephyr-arm-toolchain.tar.xz

# ---------------------------------------------------------------------------
# Register SDK (CMake package registry) and install the host tools
# ---------------------------------------------------------------------------

RUN "${ZEPHYR_SDK_INSTALL_DIR}/setup.sh" -h -c

# ---------------------------------------------------------------------------
# SEGGER J-Link Software (optional)
#
# The J-Link Software cannot be redistributed and is downloaded only after
# accepting SEGGER's license. Download "J-Link Software and Documentation
# Pack", Linux, 64-bit, TGZ archive from https://www.segger.com/downloads/jlink/
# and put JLink_Linux_V*_x86_64.tgz into docker/jlink/ before building the
# image. Without the archive the image has no J-Link tools (the OpenOCD
# runner and the host-side J-Link GDB server keep working).
# ---------------------------------------------------------------------------

COPY docker/jlink/ /tmp/jlink/
RUN set -eux; \
    archive="$(ls /tmp/jlink/JLink_Linux_V*_x86_64.tgz 2>/dev/null | sort | tail -n 1 || true)"; \
    if [ -n "${archive}" ]; then \
        mkdir -p /opt/SEGGER/JLink; \
        tar -xzf "${archive}" --strip-components=1 -C /opt/SEGGER/JLink; \
        echo "Installed $(basename "${archive}")"; \
    else \
        echo "No J-Link archive in docker/jlink/: J-Link tools not installed"; \
    fi; \
    rm -rf /tmp/jlink

# ---------------------------------------------------------------------------
# Workspace
#
# West workspace (topdir) /workdir: zephyr/ and modules/ live in a named
# volume mounted there, this repository is mounted at /workdir/doppelstep.
# ---------------------------------------------------------------------------

RUN mkdir -p /workdir/.ccache \
    && ccache --set-config=max_size=2G \
    && ccache --set-config=compression=true

WORKDIR /workdir/doppelstep

# Do not copy project sources into the image.
# The workspace is mounted at runtime.

CMD ["/bin/bash"]
