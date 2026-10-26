#!/bin/sh
# Fetch dynarmic at the version this port is built against and apply the Switch patch.
# usage: tools/fetch_deps.sh   (from the repository root)
set -e
cd "$(dirname "$0")/.."
COMMIT=a46601580d5512d324104f985b5f0209dc980ddc   # azahar-emu/dynarmic, "Delete redundant add_library alias"
if [ ! -d ext/dynarmic ]; then
    mkdir -p ext
    git clone https://github.com/azahar-emu/dynarmic ext/dynarmic
fi
git -C ext/dynarmic checkout -q "$COMMIT"
git -C ext/dynarmic submodule update --init externals/fmt externals/mcl externals/oaknut externals/robin-map externals/xbyak externals/zycore externals/zydis
if patch -p1 -d ext/dynarmic --dry-run -s < patches/dynarmic-switch.patch >/dev/null 2>&1; then
    patch -p1 -d ext/dynarmic -s < patches/dynarmic-switch.patch
    echo "dynarmic ready (Switch patch applied)"
else
    echo "dynarmic ready (patch already applied)"
fi
