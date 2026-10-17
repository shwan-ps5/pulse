#!/usr/bin/env bash
# Builds Pulse as a native PS5 title on the ps5-opengl SDK (OpenGL 4.6 via EGL); TARGET=hands builds the controller test:
#   bash ps5/build.sh     -> build/ps5/dist/PPSA99993 (+ .zip)
# The boilerplate's build runs in a private copy, because a GL title needs ps5-opengl's linker script and
# symbol map, a fixed 256 MiB heap, every allocator call wrapped onto its app heap, its GPU import
# stubs, and the C++ runtime Mesa needs.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)
boilerplate=${BOILERPLATE:-$HOME/dev/ps5-native-app-boilerplate}
gl_sdk=${PS5_OPENGL_SDK:-$HOME/dev/ps5-opengl-sdk/ps5-opengl-sdk-1.0.1/sdk}
gl_src=${PS5_OPENGL_SRC:-$HOME/dev/ps5-opengl}
stage=$root/build/ps5

for need in "$boilerplate/tools/build.sh" "$boilerplate/.deps/native/ps5-payload-sdk/bin/prospero-lld" \
    "$gl_sdk/lib/libPS5OpenGL.a" "$gl_src/native-app/runtime_shims.c"; do
    [[ -e $need ]] || { echo "missing: $need" >&2; exit 2; }
done

rm -rf "$stage"
mkdir -p "$stage"/{src,include,vendor,assets}
for dir in runtime sce_sys tooling tools .deps/native; do
    mkdir -p "$stage/$(dirname "$dir")"
    cp -a "$boilerplate/$dir" "$stage/$dir"
done

# TARGET=game (default) builds Pulse with the song packaged in the title; TARGET=hands the controller test.
target=${TARGET:-game}
cp "$root"/platform/{platform_ps5.cpp,vr_ps5.cpp,vr_eyes.cpp,*.h} "$stage/src/"
if [[ $target == hands ]]; then
    cp "$root/test/hands.cpp" "$stage/src/main.cpp"
else
    song=${PULSE_SONG:-$root/ps5/demo/Pulse Demo - Neon Run.mp3}
    [[ -e $song ]] || { echo "missing: $song (PULSE_SONG)" >&2; exit 2; }
    cp "$root/game/main.cpp" "$stage/src/main.cpp"
    cp "$root"/game/{library.cpp,library.h,scores.cpp,scores.h,tags.cpp,tags.h} "$stage/src/"
    cp "$root"/core/{game.cpp,game.h,bot.h} "$root"/render/{render.cpp,render.h,fontatlas.h} "$root"/analysis/{analysis.cpp,analysis.h,mapper.cpp,mapper.h} \
        "$root"/audio/{mp3.cpp,mp3.h,minimp3.h} "$root"/third_party/stb_image.h "$stage/src/"
    mkdir -p "$stage/assets/songs"
    cp "$song" "$stage/assets/songs/$(basename "$song")"
    python3 "$root/tools/make-index.py" "$stage/assets/songs" >/dev/null
fi
cp "$here/param.json" "$stage/sce_sys/param.json"
# Where the title sends its live log (tools/logsrv.py listens on UDP 9023): this PC unless PULSE_LOGHOST says otherwise.
# A release (PULSE_RELEASE=1) carries no address; the title then does not log over the network.
if [[ -z ${PULSE_RELEASE:-} ]]; then
    echo "${PULSE_LOGHOST:-$(hostname -I | awk '{print $1}')}" > "$stage/assets/loghost.txt"
fi
cp "$here/libc_shims.c" "$gl_src/native-app/runtime_shims.c" "$gl_src/native-app/app_heap.c" "$stage/src/"
# Launcher art from tools/makeart.py (.venv/bin/python tools/makeart.py ps5/art <a .dds to borrow the header from>).
rm -f "$stage/sce_sys/snd0.at9"
cp "$here"/art/{icon0.png,pic0.dds,pic1.dds} "$stage/sce_sys/"
cp -a "$gl_sdk/include/." "$stage/include/"

# ps5-opengl's linker script INCLUDEs the boilerplate's own under this name.
cp "$stage/tooling/native/ps5-pie.ld" "$stage/tooling/native/ps5-pie-base.ld"
cp "$gl_src/native-app/ps5-pie.ld" "$gl_src/native-app/app-symbols.map" "$stage/tooling/native/"
replace_once() {
    local file=$1 old=$2 new=$3
    [[ $(grep -cF -- "$old" "$file") == 1 ]] || { echo "boilerplate changed: $file: $old" >&2; exit 2; }
    python3 -I -c 'import sys; p,o,n=sys.argv[1:]; s=open(p).read(); open(p,"w").write(s.replace(o,n,1))' "$file" "$old" "$new"
}
replace_once "$stage/tooling/native/sce_module_writer.cpp" \
    'write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());' \
    'write_u64(result.data, result.heap_size, 0x10000000ULL);'
replace_once "$stage/tools/build.sh" 'bash "$root/tools/setup-native-dependencies.sh" >/dev/null' \
    'test -x "$root/.deps/native/ps5-payload-sdk/bin/prospero-lld"'
replace_once "$stage/tools/build.sh" '--eh-frame-hdr \' \
    '--eh-frame-hdr --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size \'

payload_sdk=$stage/.deps/native/ps5-payload-sdk
cp "$gl_sdk/lib/libSceAgc.so" "$gl_sdk/lib/libSceAgcDriver.so" "$payload_sdk/target/lib/"
compiler_rt=$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a
cat > "$stage/vendor/libpulse-group.a" <<GROUP
SEARCH_DIR("$payload_sdk/target/lib")
SEARCH_DIR("$gl_sdk/lib")
EXTERN(ps5_agc_gate2_run)
GROUP (
  "$gl_sdk/lib/libPS5OpenGL.a"
  "$payload_sdk/target/lib/libunwind.a"
  "$payload_sdk/target/lib/libc++abi.a"
  "$payload_sdk/target/lib/libc++.a"
  "$compiler_rt"
)
GROUP

cd "$stage"
APP_SOURCE_DIR=src APP_PARAM=sce_sys/param.json APP_SCE_SYS=sce_sys APP_ASSETS=assets \
APP_INCLUDE_PATHS=include APP_STATIC_ARCHIVES=vendor/libpulse-group.a \
APP_DEFINITIONS="PLATFORM_PS5 _GNU_SOURCE GL_GLEXT_PROTOTYPES=1${PULSE_RELEASE:+ PULSE_RELEASE}" \
PACBREW_PACKAGES= PACBREW_INCLUDE_PATHS= PACBREW_STATIC_ARCHIVES= APP_RUNTIME_MODULES= \
    bash tools/build.sh Folder
