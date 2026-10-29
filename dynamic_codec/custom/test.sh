#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
DEP_ROOT=${DEP_ROOT:-../../deps/root}
PROTO_GEN_DIR=${PROTO_GEN_DIR:-../../codec}
CXX=${CXX:-g++}
mkdir -p build
"$CXX" -std=c++20 -O2 -Wall -Wextra -Wpedantic -I"$DEP_ROOT/usr/include" -I"$PROTO_GEN_DIR" functional_unit.cpp "$PROTO_GEN_DIR/event.pb.cc" -Wl,-l:libprotobuf.so.32 -pthread -o build/functional_unit
./build/functional_unit
