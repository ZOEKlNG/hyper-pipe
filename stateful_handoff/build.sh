#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
DEP_ROOT=${DEP_ROOT:-"$PWD/../deps/root"}
export LD_LIBRARY_PATH="$DEP_ROOT/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
if [[ ! -f events.pb.cc ]]; then
  "$DEP_ROOT/usr/bin/protoc" --cpp_out=. events.proto
fi
CMAKE=${CMAKE:-cmake}
"$CMAKE" -S . -B build -DCMAKE_BUILD_TYPE=Release -DDEP_ROOT="$DEP_ROOT"
"$CMAKE" --build build -j 1
