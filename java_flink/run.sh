#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
exec java -Xms512m -Xmx1536m -Dorg.slf4j.simpleLogger.defaultLogLevel=warn --add-opens=java.base/java.lang=ALL-UNNAMED --add-opens=java.base/java.util=ALL-UNNAMED --add-opens=java.base/java.nio=ALL-UNNAMED -cp "classes:$(cat classpath.txt)" "$@"
