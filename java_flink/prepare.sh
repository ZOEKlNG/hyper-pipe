#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
if [ -z "${MVN:-}" ] && [ ! -x apache-maven-3.9.9/bin/mvn ]; then
  curl -fL --retry 2 https://repo.maven.apache.org/maven2/org/apache/maven/apache-maven/3.9.9/apache-maven-3.9.9-bin.tar.gz -o maven.tar.gz
  tar xzf maven.tar.gz
fi
if [ ! -f ecj.jar ]; then
  curl -fL --retry 2 https://repo.maven.apache.org/maven2/org/eclipse/jdt/ecj/3.40.0/ecj-3.40.0.jar -o ecj.jar
fi
python3 - <<'PY'
from pathlib import Path
import os,urllib.parse,xml.sax.saxutils
p=Path.cwd();proxy=urllib.parse.urlparse(os.environ.get('HTTPS_PROXY',''))
proxy_xml=''
if proxy.hostname:
    proxy_xml=f'<proxies><proxy><id>session</id><active>true</active><protocol>http</protocol><host>{xml.sax.saxutils.escape(proxy.hostname)}</host><port>{proxy.port or 8080}</port></proxy></proxies>'
p.joinpath('settings.xml').write_text(f'<settings><localRepository>{p}/m2</localRepository>{proxy_xml}</settings>')
PY
if [ "${1:-}" != "--tools-only" ]; then
  bash build.sh
fi
