#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
DEP_ROOT=${DEP_ROOT:-"$PWD/../deps/root"}
CXX=${CXX:-g++}
CC=${CC:-gcc}
mkdir -p .build results
flags=(-O3 -DNDEBUG -std=c++23 -march=native -pthread -Ivendor/glaze/include -I../deps/root/usr/include)
libs=(-lprotobuf)
if [[ -f "$DEP_ROOT/usr/include/google/protobuf/message.h" ]]; then
  flags+=(-I"$DEP_ROOT/usr/include")
  libs=(-L"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -Wl,-rpath,"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -lprotobuf)
fi
"$CC" -O3 -DNDEBUG -march=native -std=c11 -c vendor/yyjson/yyjson.c -o .build/yyjson.o
for src in nlohmann_adapter.cpp yyjson_adapter.cpp glaze_adapter.cpp protobuf_adapter.cpp bench.cpp; do
  "$CXX" "${flags[@]}" -c "$src" -o ".build/${src%.cpp}.o"
done
"$CXX" "${flags[@]}" -c ../core_handoff/log.pb.cc -o .build/log.pb.o
"$CXX" -pthread .build/*.o "${libs[@]}" -o bench
