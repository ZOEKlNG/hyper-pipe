#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# Reuse frozen generated protobuf sources. Do not rerun protoc or edit primary files.
DEP_ROOT=${DEP_ROOT:-/workspace/work/deps/root}
g++ -O3 -DNDEBUG -std=c++20 -march=native -pthread -Wall -Wextra -I"$DEP_ROOT/usr/include" handoff_cpu2.cpp log.pb.cc -o handoff_cpu2 -L"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -Wl,-rpath,"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -lprotobuf
