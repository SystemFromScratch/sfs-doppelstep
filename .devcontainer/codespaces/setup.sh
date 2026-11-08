#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Codespaces: make the parent of the repository the west topdir, on the west
# workspace baked into the environment image (/workdir, stage ci of the
# Dockerfile).
#
# Codespaces clones the repository to /workspaces/<repository>, while the image
# holds the workspace in /workdir with the manifest repository expected at
# /workdir/doppelstep. Instead of fetching Zephyr again, the projects of the
# image are linked into /workspaces and a west configuration there points at
# the clone:
#
#   /workspaces/.west/config     manifest path = <repository>
#   /workspaces/zephyr      ->   /workdir/zephyr
#   /workspaces/modules     ->   /workdir/modules
#   /workspaces/bootloader  ->   /workdir/bootloader
#   /workdir/doppelstep     ->   /workspaces/<repository>

set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
topdir="$(dirname "${repo}")"
image=/workdir

[ -d "${image}/.west" ] || {
    echo "ERROR: no west workspace in ${image}: use the image of .devcontainer/codespaces" >&2
    exit 1
}

# The Zephyr build runs west from the real path of Zephyr (/workdir/zephyr),
# where it finds the topdir /workdir and its manifest at /workdir/doppelstep:
# point that at the clone too.
if [ -d "${image}/doppelstep" ] && [ ! -L "${image}/doppelstep" ]; then
    rmdir "${image}/doppelstep"
fi
ln -sfn "${repo}" "${image}/doppelstep"

for dir in zephyr modules bootloader; do
    if [ -e "${image}/${dir}" ] && [ ! -e "${topdir}/${dir}" ]; then
        ln -s "${image}/${dir}" "${topdir}/${dir}"
    fi
done

mkdir -p "${topdir}/.west"
cat > "${topdir}/.west/config" <<EOF
[manifest]
path = $(basename "${repo}")
file = west.yml

[zephyr]
base = zephyr
EOF

git config --global --add safe.directory '*'
ccache --zero-stats > /dev/null

cd "${repo}"
west list

if ! cmp -s "${repo}/west.yml" "${image}/.west/west.yml.image"; then
    echo
    echo "NOTE: west.yml differs from the one the image was built with."
    echo "      Run 'west update --narrow --fetch-opt=--tags' before building."
fi
echo
echo "Doppelstep workspace ready in ${topdir}. Build the board:"
echo "  west build -p always --sysbuild -b b_g431b_esc1 -s app -d build"
echo "or run the tests: bash scripts/run-tests.sh"
