#!/usr/bin/env bash
# Builds SDL2 (PS5 drivers) and the console glue as static archives into build/ps5-title-deps/.
#   tools/ps5/build_title_deps.sh      (needs deps/ps5-vulkan bootstrapped, deps/sdl2-ps5 patched, deps/ps5-doom3)
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
deps=$root/../deps
sdk=$(realpath "$deps/ps5-vulkan/.deps/native/ps5-payload-sdk")
out=$root/build/ps5-title-deps
export PS5_PAYLOAD_SDK=$sdk
cmake -S "$here/title_deps" -B "$out" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$sdk/toolchain/prospero.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DPS5_PORT_DIR="$(realpath "$deps/ps5-doom3")" -DSDL_DIR="$(realpath "$deps/sdl2-ps5")" \
    -DCMAKE_C_FLAGS="-I$(realpath "$deps/ps5-doom3")/ps5/include" > "$out.configure.log" 2>&1 || { tail -30 "$out.configure.log"; exit 1; }
ninja -C "$out" ps5_platform SDL2-static
find "$out" -name '*.a' -printf '%s %p\n'
