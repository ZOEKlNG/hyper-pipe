#!/usr/bin/env bash
# Portable build entry point; original build.sh/build_cpu2.sh are kept as evidence.
set -euo pipefail
cd "$(dirname "$0")"
CXX=${CXX:-g++}
DEP_ROOT=${DEP_ROOT:-"$PWD/../deps/root"}
flags=(-O3 -DNDEBUG -std=c++20 -march=native -pthread -Wall -Wextra)
libs=(-lprotobuf)
if [[ -f "$DEP_ROOT/usr/include/google/protobuf/message.h" ]]; then
  flags+=(-I"$DEP_ROOT/usr/include")
  libs=(-L"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -Wl,-rpath,"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -lprotobuf)
  export LD_LIBRARY_PATH="$DEP_ROOT/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
  PROTOC=${PROTOC:-"$DEP_ROOT/usr/bin/protoc"}
else
  PROTOC=${PROTOC:-protoc}
fi
if [[ ${REGENERATE_PROTO:-0} == 1 ]]; then
  "$PROTOC" --cpp_out=. log.proto
fi
mkdir -p .build
"$CXX" "${flags[@]}" -c log.pb.cc -o .build/log.pb.o
for program in handoff handoff_cpu2 micro drop_demo; do
  "$CXX" "${flags[@]}" "$program.cpp" .build/log.pb.o "${libs[@]}" -o "$program"
done
