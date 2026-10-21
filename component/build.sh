#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build fixtures
c++ -std=c++20 -O3 -Wall -Wextra -pthread producer.cpp -o build/producer
c++ -std=c++20 -O2 -Wall -Wextra protocol_test.cpp -o build/protocol_test
if command -v javac >/dev/null 2>&1; then javac --release 21 -d build UdpReceiver.java
else java -jar "${ECJ_JAR:-../java_flink/ecj.jar}" -21 -d build UdpReceiver.java
fi
build/protocol_test fixtures
java -cp build UdpReceiver --selftest fixtures
