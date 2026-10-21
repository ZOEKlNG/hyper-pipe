#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
RESULT_DIR=${RESULT_DIR:-"$(pwd)/../results"}
mkdir -p "$RESULT_DIR" corpus
for benchmark in bench flat_direct compression compression_reuse; do
 case "$benchmark" in bench) result_stem=codec ;; flat_direct) result_stem=flat_direct ;; *) result_stem=$benchmark ;; esac
 cat /sys/fs/cgroup/cpu.stat > "$RESULT_DIR/${result_stem}-cpu-before.txt"
 "./$benchmark" > "$RESULT_DIR/${result_stem}_raw.csv" 2> "$RESULT_DIR/${result_stem}.log"
 cat /sys/fs/cgroup/cpu.stat > "$RESULT_DIR/${result_stem}-cpu-after.txt"
done
python3 summarize.py "$RESULT_DIR"
