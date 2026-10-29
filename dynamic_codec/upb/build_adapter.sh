#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -f build/libofficial_upb_v29_3.a ]; then
  python3 build_runtime.py
fi
g++ -O3 -DNDEBUG -march=native -std=c++20 -Wall -Wextra -Iprotobuf-29.3 -I. -c adapter.cpp -o build/adapter.o
g++ -O3 -DNDEBUG -march=native -std=c++20 -Wall -Wextra -Iprotobuf-29.3 -I. smoke.cpp build/adapter.o build/libofficial_upb_v29_3.a -o build/smoke
./build/smoke
