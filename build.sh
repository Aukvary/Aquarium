#!/usr/bin/env bash
set -euo pipefail

export CCACHE_BASEDIR="$PWD"
export CCACHE_NOHASHDIR=1

psql 'postgresql://admin:password@127.0.0.1:5432/aquarium' -v ON_ERROR_STOP=1 -f sql/users.sql

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
