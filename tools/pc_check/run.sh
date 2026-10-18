#!/bin/sh
# Builds and runs the PC checks of the Vita-side code with the host compiler:
#   icon_check       vita/ExeIcon.cpp on the original EXE (compare with
#                    touhou-modern-vita tools/extract_exe_icons.py)
#   midi_blob_check  vita/MidiBuildGame.cpp + vita/MidiBuild.cpp + src/pbg4 on
#                    th07.dat and gm.dls (compare with dls_subset_tool)
# Needs g++, zlib and SDL2 development files.
# Usage: tools/pc_check/run.sh <th07.exe> <th07.dat> <gm.dls> [outdir]
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${4:-$(mktemp -d)}
mkdir -p "$out"
tdls="$root/third_party/touhou_dls_common"
c="g++ -std=c++17 -O2 -I$root/src -I$root/vita -I$tdls/include"
$c "$root/tools/pc_check/icon_check.cpp" "$root/vita/ExeIcon.cpp" "$tdls/src/sha256.cpp" -lz -o "$out/icon_check"
$c -DTH07_VITA=1 $(sdl2-config --cflags | sed 's|-I[^ ]*SDL2||') "$root/tools/pc_check/midi_blob_check.cpp" \
   "$root/tools/pc_check/host_shims.cpp" "$root/vita/MidiBuildGame.cpp" "$root/vita/MidiBuild.cpp" "$root"/src/pbg4/*.cpp \
   "$tdls/src/blob.cpp" "$tdls/src/dls.cpp" "$tdls/src/manifest.cpp" "$tdls/src/sha256.cpp" \
   "$tdls/src/smf_scan.cpp" "$tdls/src/subset.cpp" $(sdl2-config --libs) -o "$out/midi_blob_check"
"$out/icon_check" "$1" "$out/icon0-from-exe.png"
"$out/midi_blob_check" "$2" "$3" "$out/midi-th07.tdsb" 2>/dev/null
echo "outputs (personal use only, do not commit): $out"
