#!/usr/bin/env bash
#
# Builds and runs the light test bank, and puts the pictures somewhere you can look
# at them. Arguments pass straight through:
#
#   tools/lightscenes.sh                  everything, into build/scenes
#   tools/lightscenes.sh --checks         the closed-form checks alone
#   tools/lightscenes.sh --only 08        one scene, by name
#   tools/lightscenes.sh --out /somewhere pictures elsewhere
#
# This needs a graphics device: the solver is compute shaders and wants an OpenGL
# 4.3 context, so it opens a window and ignores it. Headless, it will refuse rather
# than quietly return black.

set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -f build/CMakeCache.txt ]; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
fi

cmake --build build --target LightScenes

mkdir -p build/scenes

# Single-config on Ninja and Makefiles, but Xcode and friends put it in a
# per-config subdirectory.
if [ -x build/LightScenes ]; then
    build/LightScenes "$@"
else
    build/Release/LightScenes "$@"
fi

echo
echo "pronto: build/scenes/00-contact-sheet.png"
