#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Initialise (first run) and update the west workspace inside the container.
# Topdir: the parent of this repository (/workdir in the dev container).
#
# --fetch-opt=--tags also fetches the release tags of the libraries, so that
# their CMake version scripts (git describe) report the real version.
#
# WEST_UPDATE=0 keeps an existing workspace as it is: GitHub Actions runs on
# the workspace baked into the ci image, fetched with the same west.yml (the
# image tag hashes it).

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
topdir="$(dirname "${repo}")"

if [ ! -d "${topdir}/.west" ]; then
    echo "west: initialising ${topdir} from ${repo}/west.yml"
    west init -l "${repo}"
fi

cd "${topdir}"
if [ "${WEST_UPDATE:-1}" = 0 ]; then
    echo "west: WEST_UPDATE=0, keeping the workspace in ${topdir}"
else
    west update --narrow --fetch-opt=--tags
fi
west list
