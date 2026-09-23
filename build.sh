#!/usr/bin/env bash
set -euo pipefail
export CCACHE_BASEDIR="$PWD" CCACHE_NOHASHDIR=1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4