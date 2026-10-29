#!/usr/bin/env python3
"""Rerun service CPU and paced drop experiments sequentially; overwrites results."""
from pathlib import Path
from datetime import datetime, timezone
import hashlib
import json
import subprocess
import time

root = Path(__file__).resolve().parent
out = root / "results"
out.mkdir(exist_ok=True)
sources = ["micro.cpp", "micro", "drop_demo.cpp", "drop_demo", "handoff.cpp",
           "common.hpp", "log.proto", "log.pb.cc", "log.pb.h", "build_all.sh"]

def snapshot():
    return {p: hashlib.sha256((root / p).read_bytes()).hexdigest() for p in sources}

before = snapshot()
(out / "service_snapshot_before.json").write_text(json.dumps(before, indent=2) + "\n")
timing = {}
for name in ["micro", "drop_demo"]:
    stat = Path("/sys/fs/cgroup/cpu.stat")
    (out / f"{name}_cpu_before.txt").write_text(stat.read_text())
    start = time.monotonic()
    started = datetime.now(timezone.utc).isoformat()
    with (out / f"{name}.stdout").open("w") as stdout, (out / f"{name}.stderr").open("w") as stderr:
        proc = subprocess.run([f"./{name}"], cwd=root, stdout=stdout, stderr=stderr)
    timing[name] = {"wall_seconds": time.monotonic() - start, "returncode": proc.returncode,
                    "started_utc": started, "command": [f"./{name}"]}
    (out / f"{name}_cpu_after.txt").write_text(stat.read_text())
    (out / "service_timing.json").write_text(json.dumps(timing, indent=2) + "\n")
    if proc.returncode:
        raise SystemExit(proc.returncode)
after = snapshot()
(out / "service_snapshot_after.json").write_text(json.dumps(after, indent=2) + "\n")
if before != after:
    raise SystemExit("Measured files changed during run")
print(json.dumps(timing))
