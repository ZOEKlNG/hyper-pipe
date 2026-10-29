#!/bin/bash
# Source from the unified harness build, do not execute as a separate process.
upb_adapter_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UPB_CXXFLAGS=("-I${upb_adapter_dir}/protobuf-29.3")
UPB_SOURCES=("${upb_adapter_dir}/adapter.cpp")
UPB_LDFLAGS=("${upb_adapter_dir}/build/libofficial_upb_v29_3.a")
