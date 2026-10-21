#!/bin/bash
set -euo pipefail
SBE_DIR=$(cd "$(dirname "$0")" && pwd)
SBE_OUT=${1:-"$SBE_DIR"}
mkdir -p "$SBE_OUT"
SBE_OUT=$(cd "$SBE_OUT" && pwd)
export BENCH_CORPUS_DIR=${BENCH_CORPUS_DIR:-"$SBE_DIR/../codec/corpus"}
cd "$SBE_DIR"
./bench_validated > "$SBE_OUT/sbe_validated_raw.csv" 2> "$SBE_OUT/sbe_validated.log"
./bench_trusted > "$SBE_OUT/sbe_trusted_raw.csv" 2> "$SBE_OUT/sbe_trusted.log"
python3 summarize.py "$SBE_OUT"
