#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build "$@"

echo
echo "pronto: ./build/CppGame"
