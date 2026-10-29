#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
DEP_ROOT=${DEP_ROOT:-"$(pwd)/../deps/root"}
flags=(-O3 -DNDEBUG -std=c++20 -march=native -Wall -Wextra -I"$DEP_ROOT/usr/include")
links=(-L"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -Wl,-rpath,"$DEP_ROOT/usr/lib/x86_64-linux-gnu" -lprotobuf -pthread)
sources=(bench.cpp ../codec/event.pb.cc)
if [[ -f custom/codecs.hpp ]]; then flags+=(-DHAVE_CUSTOM); fi
if [[ -f upb/link_flags.sh ]]; then
 source upb/link_flags.sh
 flags+=(-DHAVE_UPB "${UPB_CXXFLAGS[@]}")
 sources+=("${UPB_SOURCES[@]}")
 links+=("${UPB_LDFLAGS[@]}")
fi
g++ "${flags[@]}" "${sources[@]}" -o bench "${links[@]}"
