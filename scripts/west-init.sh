#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Initialise (first run) and update the west workspace inside the container.
# Topdir: the parent of this repository (/workdir in the dev container).
#
# --fetch-opt=--tags also fetches the release tags of the libraries, so that
# their CMake version scripts (git describe) report the real version.

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
topdir="$(dirname "${repo}")"

if [ ! -d "${topdir}/.west" ]; then
    echo "west: initialising ${topdir} from ${repo}/west.yml"
    west init -l "${repo}"
fi

cd "${topdir}"
west update --narrow --fetch-opt=--tags
west list
