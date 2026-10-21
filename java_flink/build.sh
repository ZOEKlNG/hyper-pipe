#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
bash prepare.sh --tools-only
maven_command="${MVN:-./apache-maven-3.9.9/bin/mvn}"
"$maven_command" -q -s settings.xml org.apache.maven.plugins:maven-dependency-plugin:3.8.1:build-classpath -Dmdep.outputFile=classpath.txt
python3 - <<'PY'
from pathlib import Path
import re
entries=Path('classpath.txt').read_text().split(':')
for artifact, version in [('flink-core','2.1.0'),('flink-streaming-java','2.1.0'),('flink-clients','2.1.0'),('protobuf-java','3.21.12')]:
    matching=[e for e in entries if re.fullmatch(re.escape(artifact)+r'-\d.*\.jar',Path(e).name)]
    expected=artifact+'-'+version+'.jar'
    if len(matching)!=1 or Path(matching[0]).name!=expected:
        raise SystemExit(f'Dependency mismatch for {artifact}; expected {expected}')
print('Pinned Flink 2.1.0 / Protobuf Java 3.21.12 dependencies verified')
PY
mkdir -p classes
find src/main/java generated -name '*.java' > sources.txt
java -jar ecj.jar -17 -nowarn -cp "$(cat classpath.txt)" -d classes @sources.txt
