#!/usr/bin/env bash
# Links objects into a signed PS5 title folder with the RADV archive (the PS5_Vulkan recipe, deps/ps5-vulkan).
#
#   title_link.sh TITLE_DIR PARAM_JSON ICON_PNG OBJECT...
#
# Needs deps/ps5-vulkan bootstrapped (RADV archive, native tool, libc.prx) and LLD 22: run through
# LLVM_CONFIG=<shim reporting a bin dir whose ld.lld is 22> (docs/ps5.md). The title ends up in TITLE_DIR:
# eboot.bin, sce_sys/{param.json,icon0.png}, sce_module/libc.prx. The deps/ recipe is GPL-3.0: it is called, not copied.
set -euo pipefail
title_dir=$1 param=$2 icon=$3
shift 3
objects=("$@")
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
deps=${PS5_DEPS:-$(cd -- "$here/../../.." && pwd)/deps}
vulkan=${PS5_VULKAN_DIR:-$deps/ps5-vulkan}
# The SDK pin the template builds with (611893f): its platform layer has the libc names (readlink, mkstemp, link...) PS5_Vulkan's older pin lacks.
sdk=${PS5_SDK_DIR:-$vulkan/.deps/native/ps5-payload-sdk}
archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
tool=$vulkan/build/host/ps5-native-tool
native=$vulkan/tooling/native
work=${PS5_TITLE_WORK:-$here/../../build/ps5-title-work}
mkdir -p "$work/obj" "$work/stubs"
extra_stub_args=()
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }

cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections -c "$native/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
stub() {
    cc -std=c11 -O2 -fPIC -c "$vulkan/$2" -o "$work/obj/$1_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "$1.prx" -o "$work/stubs/$1.so" "$work/obj/$1_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# Extra stub libraries (PS5_EXTRA_STUBS="libSceMouse=/path/stub.c ..."): imports a system module provides that the SDK has no stub for.
extra_stubs=()
for pair in ${PS5_EXTRA_STUBS:-}; do
    cc -std=c11 -O2 -fPIC -c "${pair#*=}" -o "$work/obj/${pair%%=*}_stub.o"
    "$sdk/bin/prospero-lld" --shared -soname "${pair%%=*}.prx" -o "$work/stubs/${pair%%=*}.so" "$work/obj/${pair%%=*}_stub.o"
    extra_stubs+=("$work/stubs/${pair%%=*}.so")
    extra_stub_args+=(--stub "$work/stubs/${pair%%=*}.so")
done

# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$archive" || exit 2
# PS5_UNBIND="name ...": names the recipe binds to the platform layer (--defsym=name=ps5_name) that the caller binds itself.
for name in ${PS5_UNBIND:-}; do
    kept=()
    for flag in "${radv_link_flags[@]}"; do [[ $flag == "--defsym=$name=ps5_$name" ]] || kept+=("$flag"); done
    radv_link_flags=("${kept[@]}")
done
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$native/app-symbols.map" --exclude-libs=ALL -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/app_crt.o" "$work/obj/app_cpp_runtime.o" "${objects[@]}" \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" "${extra_stubs[@]}" "${radv_link_inputs[@]}" \
    --as-needed "$sdk"/target/lib/*.so
# An import that no module a title loads exports is null at run time (its first call jumps to address 0). libkernel_sys's and
# libScePosixForWebKit's exports are not loaded by a title either. Listed, and refused unless PS5_ALLOW_NULL_IMPORTS is set.
null_imports=$(comm -23 \
    <("$sdk/bin/llvm-nm" -D --undefined-only "$work/llvm-pie.elf" | awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk"/target/lib/*.so "$work"/stubs/*.so; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    printf '%s\n' "$null_imports" > "$work/null-imports.txt"
    echo "title_link.sh: $(wc -l < "$work/null-imports.txt") imports no loaded module exports (null at run time), listed in $work/null-imports.txt:" >&2
    head -40 "$work/null-imports.txt" | tr '\n' ' ' >&2; echo >&2
    [[ -n ${PS5_ALLOW_NULL_IMPORTS:-} ]] || exit 1
fi
"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    --stub "$work/stubs/libSceAgc.so" --stub "$work/stubs/libSceAgcDriver.so" "${extra_stub_args[@]}" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
rm -rf -- "$title_dir"
mkdir -p "$title_dir/sce_sys" "$title_dir/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$title_dir/eboot.bin" --magic 0x1D3D154F
cp "$param" "$title_dir/sce_sys/param.json"
cp "$icon" "$title_dir/sce_sys/icon0.png"
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$title_dir/sce_module/libc.prx"
"$tool" self --inspect --file "$title_dir/eboot.bin" > /dev/null
printf 'title: %s (eboot.bin %s bytes)\n' "$title_dir" "$(stat -c %s "$title_dir/eboot.bin")"
