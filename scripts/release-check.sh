#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/release-check
CXX="${CXX:-g++}"
FLAGS=(-std=c++20 -O2 -Wall -Wextra -Wpedantic)
"$CXX" "${FLAGS[@]}" server/main.cpp -o build/release-check/v1
"$CXX" "${FLAGS[@]}" -pthread server/worker_pool.cpp -o build/release-check/v2
"$CXX" "${FLAGS[@]}" -I"$(pg_config --includedir)" server/persistent_server.cpp -lpq -o build/release-check/persistent
"$CXX" "${FLAGS[@]}" -I"$(pg_config --includedir)" server/observable_server.cpp -lpq -o build/release-check/observable
"$CXX" "${FLAGS[@]}" -I"$(pg_config --includedir)" server/idempotent_server.cpp -lpq -o build/release-check/idempotent
python3 -m py_compile tests/test_idempotency.py experiments/009-multi-failure/probe.py
printf 'PASS: 5 C++ builds, 2 Python syntax checks\n'
printf 'NOTE: PostgreSQL end-to-end tests are NOT run by this script.\n'
