#!/usr/bin/env bash
#
# Draws a contact sheet of the generated plants, at one screen pixel per texel
# times the zoom, and reports the sizes they came out against the sizes the table
# asks for.
#
# It compiles the few sources it needs directly rather than going through CMake:
# the probe is not part of the game and should not be a target of it, and the
# whole of what it needs is already sitting in build/ from the last build.
#
#   ./sheet.sh                 six of each, mature, summer, 3x
#   ./sheet.sh -n 10 -z 4      ten of each at 4x
#   ./sheet.sh -s 0            saplings   (0 sapling, 1 young, 2 mature, 3 old)
#   ./sheet.sh -e 2            autumn     (0 spring, 1 summer, 2 autumn, 3 winter)
#   ./sheet.sh -o /tmp/x.png   somewhere else
#
set -euo pipefail

cd "$(dirname "$0")"

raylib_src="build/_deps/raylib-src/src"
raylib_lib="build/_deps/raylib-build/raylib/libraylib.a"

if [[ ! -f "$raylib_lib" ]]; then
    echo "raylib nao foi construida ainda -- rode ./build.sh primeiro." >&2
    exit 1
fi

mkdir -p build

# Rebuilt whenever anything it draws from has changed. The probe exists to be
# run immediately after an edit, so a stale binary is worse than a slow start.
# The cave sources are here because terrain reads the cave fields while deciding
# what is solid, and the picture sources because the strips under the plants are
# drawn from the item and element tables.
newest=$(ls -t tools/sheet.cpp src/canopy.cpp src/flora.cpp src/terrain.cpp src/grid.cpp \
             src/cave.cpp src/picture.cpp \
             src/canopy.h src/flora.h src/config.h src/element.h src/item.h src/picture.h 2>/dev/null | head -1)

if [[ ! -x build/sheet || "$newest" -nt build/sheet ]]; then
    echo "compilando a sonda..."

    clang++ -std=c++26 -O2 -o build/sheet \
        tools/sheet.cpp src/canopy.cpp src/flora.cpp src/terrain.cpp src/grid.cpp \
        src/cave.cpp src/picture.cpp \
        -Isrc -I"$raylib_src" -I"$raylib_src/external" \
        "$raylib_lib" \
        -framework Cocoa -framework IOKit -framework CoreVideo -framework OpenGL
fi

# raylib writes its own banner to stdout, which buries the measurements.
./build/sheet "$@" 2> >(grep -v '^INFO:' >&2)

out="build/arvores.png"
for ((i = 1; i <= $#; i++)); do
    if [[ "${!i}" == "-o" ]]; then
        next=$((i + 1))
        out="${!next}"
    fi
done

echo
echo "pronto: $out"

# Opened straight away, since the point of it is to be looked at.
[[ "$(uname)" == "Darwin" ]] && open "$out" || true
