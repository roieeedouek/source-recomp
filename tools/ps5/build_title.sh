#!/usr/bin/env bash
# Links Portal 2 as a native PS5 title: build/ps5-title-game/<TITLE_ID>/ (eboot.bin, sce_sys, sce_module).
#   tools/ps5/build_title.sh [TITLE_ID]
# Needs: python3 tools/build_game.py --target ps5 --release --title (the object list, build/game/link.rsp),
#        tools/ps5/build_title_deps.sh (SDL2 + console glue), deps/ps5-vulkan bootstrapped, LLD 22 shim (docs/ps5.md).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
deps=$root/../deps
title=${1:-PPSA97622}
work=$root/build/ps5-title-game
vulkan=$deps/ps5-vulkan
sdk=$(realpath "$deps/ps5-vulkan-template/.deps/native/ps5-payload-sdk")
homebrew=$(realpath "$deps/ps5-sdk-full/target/user/homebrew/lib")
glue=$root/build/ps5-title-deps
export PS5_PAYLOAD_SDK=$sdk PS5_SDK_DIR=$sdk LLVM_CONFIG=${LLVM_CONFIG:-$root/build/ps5-toolchain/llvm-config}
mkdir -p "$work"

# dlsym table of the offline Steam stub, and the title's parameter file
if [[ -f $here/gen_dl_table.py ]]; then   # names a local build looks up with dlsym on itself; without it, an empty table
    python3 "$here/gen_dl_table.py" "$work/dltable.c" "@$root/build/game/link.rsp"
else
    printf 'struct rt_dl_entry { const char *name; void *address; };\nconst struct rt_dl_entry rt_dl_table[1];\nconst unsigned rt_dl_count = 0;\n' > "$work/dltable.c"
fi
sh "$vulkan/tooling/prospero-clang18" -std=c11 -O1 -c "$work/dltable.c" -o "$work/dltable.o"
python3 -I - "$title" "$work/param.json" "$vulkan/sce_sys/param-radv.json" <<'PY'
import json, sys
title, out, template = sys.argv[1:4]
d = json.load(open(template))
d['conceptId'] = title[4:]; d['titleId'] = title; d['contentId'] = f'UP9000-{title}_00-P2RECOMP00000001'
d['localizedParameters']['en-US']['titleName'] = 'Portal 2 (source-recomp)'
# 120 Hz output where the display takes it (RADV's video-out WSI reads this bit; it falls back to 59.94 Hz and logs why)
d['attribute3'] = d.get('attribute3', 0) | 0x40
json.dump(d, open(out, 'w'), indent=2)
PY

# libc names the title's imports cannot reach: Doom 3's shims.c answers them as ps5dl_<name> (kept local), see title_link notes
shims=(isatty getresuid getresgid dl_iterate_phdr fchown fork vfork setsid pathconf getnameinfo gai_strerror lstat clock_gettime execlp execl strsignal fnmatch gethostbyname realpath)
flags=(--wrap=fprintf --wrap=vfprintf --wrap=fputs --wrap=fputc --wrap=fwrite --wrap=sceAgcDriverSubmitDcb --wrap=sceAgcSuspendPoint --wrap=dlopen --wrap=dlsym --wrap=dlerror --wrap=dlclose --error-limit=0)
for name in "${shims[@]}"; do flags+=("--defsym=$name=ps5dl_$name"); done
# (chown/lchown/gl*: runtime/ps5_title.c and ps5_gl_stub.c define names the stub libraries also define: kept local, the converter refuses exports)
# and the platform layer's own ps5_<name> the recipe does not bind (SDK pin 611893f)
platform_names=(statfs fstatfs)
# PS5_PGO_GENERATE=1: the objects were built with --pgo-generate; LLVM's profiling runtime goes in (runtime/ps5_title.c has the
# glibc names it needs), and the counts land in /data/portal2/tmp/portal2-ps5.profraw.
pgo_flags=() pgo_local=()
if [[ ${PS5_PGO_GENERATE:-0} == 1 ]]; then
    pgo_flags=(--no-dependent-libraries -u __llvm_profile_runtime --defsym=uname=rt_title_uname "$(clang -print-resource-dir)/lib/linux/libclang_rt.profile-x86_64.a")
    pgo_local=(__errno_location __fprintf_chk __snprintf_chk __memcpy_chk prctl uname rt_title_uname '__llvm_*' 'lprof*' '__llvm_profile_*')
fi
flags+=(--defsym=towupper_l=rt_towupper_l --defsym=towlower_l=rt_towlower_l --defsym=readlink=rt_title_readlink --defsym=chdir=rt_title_chdir --defsym=getcwd=rt_title_getcwd)   # runtime/ps5_title.c: /proc/self/exe is /app0/eboot.bin; PS5_UNBIND drops the recipe's own bind
for name in "${platform_names[@]}"; do flags+=("--defsym=$name=ps5_$name"); done
{ printf '{\n    local:\n'; printf '        %s;\n' "${shims[@]}" "${platform_names[@]}" chown lchown readlink towupper_l towlower_l rt_towupper_l rt_towlower_l "${pgo_local[@]}" rt_title_readlink __wrap_sceAgcDriverSubmitDcb __wrap_sceAgcSuspendPoint __wrap_fprintf __wrap_vfprintf __wrap_fputs __wrap_fputc __wrap_fwrite chdir getcwd rt_title_chdir rt_title_getcwd 'gl*'; printf '};\n'; } > "$work/shims-local.map"
flags+=(--version-script "$work/shims-local.map")

# Title artwork: build/ps5-art (tools/ps5/make_title_art.py: icon0.png, pic0.dds, pic1.dds) when it exists, else the
# placeholder icon. Made from screenshots of the game, it stays out of the repository.
art=${PS5_TITLE_ART:-$root/build/ps5-art}
icon=$root/build/ps5-app/PPSA97620/sce_sys/icon0.png
[[ -f $art/icon0.png ]] && icon=$art/icon0.png
PS5_UNBIND=readlink PS5_EXTRA_STUBS="libSceMouse=$(realpath "$deps/ps5-doom3/ps5/stubs/libSceMouse_stub.c")" PS5_TITLE_WORK=$work/link \
"$here/title_link.sh" "$work/$title" "$work/param.json" "$icon" \
    "@$root/build/game/link.rsp" "$work/dltable.o" "${flags[@]}" "${pgo_flags[@]}" \
    --whole-archive "$glue/libps5_platform.a" --no-whole-archive "$glue/SDL2/libSDL2.a" \
    -L "$homebrew" --start-group -lopenal -lfontconfig -lfreetype -liconv -lpng16 -lexpat -lbz2 --end-group
for picture in pic0.dds pic1.dds; do
    if [[ -f $art/$picture ]]; then cp "$art/$picture" "$work/$title/sce_sys/$picture"; fi
done
