#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .deps
repo_root="$(cd ../.. && pwd)"
if [ ! -f "$repo_root/java_flink/m2/com/google/protobuf/protobuf-java/3.21.12/protobuf-java-3.21.12.jar" ] && [ ! -f .deps/protobuf-java-3.21.12.jar ]; then
  curl -fL --retry 2 https://repo.maven.apache.org/maven2/com/google/protobuf/protobuf-java/3.21.12/protobuf-java-3.21.12.jar -o .deps/protobuf-java-3.21.12.jar
fi
if [ ! -f "$repo_root/java_flink/ecj.jar" ] && [ ! -f .deps/ecj-3.40.0.jar ]; then
  curl -fL --retry 2 https://repo.maven.apache.org/maven2/org/eclipse/jdt/ecj/3.40.0/ecj-3.40.0.jar -o .deps/ecj-3.40.0.jar
fi
if [ ! -f "$repo_root/java_flink/generated/bench/EventOuterClass.java" ]; then
  printf '%s\n' 'Missing checked-in java_flink/generated/bench/EventOuterClass.java; generate from java_flink/event.proto with protoc3.21.12 or use --generated-java.' >&2
  exit 1
fi
printf '%s\n' 'Interop dependencies ready; run python3 run_interop.py --fixtures results/fixtures.input.jsonl'
