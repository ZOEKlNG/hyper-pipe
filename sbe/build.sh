#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
SBE_VERSION=1.35.0
if [ ! -f "sbe-all-${SBE_VERSION}.jar" ]; then
 curl -fL --retry 2 -o "sbe-all-${SBE_VERSION}.jar" "https://repo.maven.apache.org/maven2/uk/co/real-logic/sbe-all/${SBE_VERSION}/sbe-all-${SBE_VERSION}.jar"
fi
printf '%s  %s\n' d11f4bb7c71322e3538613b2da18b11625be400465b29a09db80bc9785cea3ab "sbe-all-${SBE_VERSION}.jar" | sha256sum -c -
java -Dsbe.target.language=Cpp -Dsbe.output.dir=generated -jar "sbe-all-${SBE_VERSION}.jar" event.xml
g++ -O3 -DNDEBUG -std=c++20 -march=native bench.cpp -o bench_validated
g++ -O3 -DNDEBUG -DSBE_NO_BOUNDS_CHECK -std=c++20 -march=native bench.cpp -o bench_trusted
./bench_validated --self-check > /dev/null
./bench_trusted --self-check > /dev/null
