#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
# An extracted user-local dependency tree is optional when system packages exist.
DEP_ROOT=${DEP_ROOT:-"$(pwd)/../deps/root"}
CXX=${CXX:-g++}
compile_flags=(-O3 -DNDEBUG -std=c++20 -march=native)
link_flags=(-pthread -lzstd -llz4)
if [[ -d "$DEP_ROOT/usr/include" ]]; then
 compile_flags+=(-I"$DEP_ROOT/usr/include")
 link_flags+=(-L"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -Wl,-rpath,"$DEP_ROOT/usr/lib/x86_64-linux-gnu")
 export LD_LIBRARY_PATH="$DEP_ROOT/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
fi
PROTOC=${PROTOC:-protoc}
FLATC=${FLATC:-flatc}
[[ ! -x "$DEP_ROOT/usr/bin/protoc" ]] || PROTOC="$DEP_ROOT/usr/bin/protoc"
[[ ! -x "$DEP_ROOT/usr/bin/flatc" ]] || FLATC="$DEP_ROOT/usr/bin/flatc"
"$PROTOC" --cpp_out=. event.proto
"$FLATC" --cpp event.fbs
"$CXX" "${compile_flags[@]}" bench.cpp event.pb.cc -o bench "${link_flags[@]}" -lprotobuf
"$CXX" "${compile_flags[@]}" flat_direct.cpp event.pb.cc -o flat_direct "${link_flags[@]}" -lprotobuf
"$CXX" "${compile_flags[@]}" compression.cpp -o compression "${link_flags[@]}"
"$CXX" "${compile_flags[@]}" compression_reuse.cpp -o compression_reuse "${link_flags[@]}"
