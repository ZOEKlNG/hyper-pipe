#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
DEP_ROOT=${DEP_ROOT:-/workspace/work/deps/root}
export LD_LIBRARY_PATH="$DEP_ROOT/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
"$DEP_ROOT/usr/bin/protoc" --cpp_out=. log.proto
g++ -O3 -DNDEBUG -std=c++20 -march=native -pthread -Wall -Wextra -I"$DEP_ROOT/usr/include" handoff.cpp log.pb.cc -o handoff -L"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -Wl,-rpath,"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -lprotobuf
