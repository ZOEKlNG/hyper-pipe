# Official upb v29.3 runtime MiniTable adapter

The runtime is the actual C implementation in `protocolbuffers/protobuf` tag
`v29.3`, peeled commit `b407e8416e3893036aee5af9a12bd9b6a0e2b2e6`.
The downloaded official archive SHA256 is
`008a11cc56f9b96679b4c285fd05f46d317d685be3ab524b2a310be0fbad987e`.
`build_metadata.json` records compiler, flags, every C translation unit and
dependencies. No system protobuf C++ internals or substitute wire parser are
used to implement upb.

Build and functional smoke (does not run a performance benchmark):

```sh
python3 build_runtime.py
bash build_adapter.sh
```

The runtime build directly compiles 34 official `.c` translation units in total,
including vendored `third_party/utf8_range/utf8_range.c`. It needs a C/C++ compiler and `ar`,
but not Bazel, CMake, Abseil, protoc, or system libprotobuf. Its upstream CMake
file in this tag is incomplete for these subdirectory targets, so the source
manifest is recorded explicitly instead of claiming a successful upstream CMake
build. Default optimization: `-O3 -DNDEBUG -march=native`, GCC 14.2.0 here.

The unified benchmark can source `link_flags.sh`, which provides the requested
`UPB_CXXFLAGS`, `UPB_SOURCES`, `UPB_LDFLAGS` Bash arrays. The resulting static library
is `build/libofficial_upb_v29_3.a`.

Build paths are computed from each script's own location and support moving the
whole `dynamic_codec/` directory, including its parent `common.hpp`. Do not commit
the downloaded source tree, tarball or `build/` outputs: they are ignored. Commit
only the adapter/build/smoke sources, README, `link_flags.sh`, and the small
`build_metadata.json` provenance record. The builder verifies archive SHA256 before
using it, and metadata records source-file and library SHA256 values as well.

## Runtime schema and the timed work

At adapter construction, the official MiniDescriptor encoder constructs the
ten-field proto3 schema and `upb_MiniTable_Build()` builds its runtime message
layout. Varint and fixed32/fixed64 schemas are supported. Field pointers are
resolved once and cached. This startup work is outside the hot measurement.
There is no generated Event MiniTable or generated upb Event accessor code.
The upstream default fast-table macro is disabled; this exercises the generic
runtime MiniTable decoder, not the performance limit of generated fast decoding.

Every normal encode/decode call includes `upb_Arena_Init`, `upb_Message_New`, and
`upb_Arena_Free`. A 64 KiB scratch region is allocated as part of the adapter
once, then reused. It is not zero-filled every operation. Only the arena/message
state that the upstream runtime initializes is initialized per operation. Passing
null as the allocator makes this arena fixed-size: it cannot silently fall back
to heap blocks. Failure is checked and reported. The adapter's maximum **payload**
of 8192 bytes is a deliberate benchmark adapter bound, not an upb library limit;
the unified corpus's payloads are at most 1024 bytes.

The schema arena is separate and lasts for the adapter lifetime. The decode
scratch is independent of the caller's immutable wire buffer. One adapter instance
is single-threaded; each benchmark worker needs its own instance.

## Ownership distinctions

`make_upb_codec(fixed, alias_decode)` uses **view input** during encode:
`upb_Message_SetString` borrows Event.payload during the synchronous encode call,
then `upb_Encode` produces arena-owned wire bytes and `out.assign` copies them into
the caller-owned `std::string` **before the arena is freed**. Field fill, real
upb encoding, final output copy and per-call arena/message lifetime are all timed.
The Event remains alive throughout the call, so no extra input copy is required
to return independently owned wire output.

`make_upb_codec_ex({fixed, alias_decode, true, false})` adds a payload copy into the
arena before field assignment. This **copy input** sensitivity variant helps
separate input-field ownership costs from serialization; it is not presented as
an unavoidable upb cost. `heap_arena=true` additionally replaces preallocated
Arena_Init with real Arena_New allocation/free per operation and is separately
labelled.

Decode's `alias_decode` switch is independent of encode's input ownership:

- `false`: upb decodes bytes payload into the arena; this copies payload bytes.
- `true`: AliasString requests a view into input wire when possible. The input
  must remain alive through all reads; tiny-buffer edge paths need not always
  yield an input alias. It is not the same ownership as a copied message.

Both modes parse the whole message, check parser status, read all requested
known scalar fields, and consume payload length + first/last byte in `decode_all`.
`decode_few` reads id + seq + type after full parse. Neither operation scans every
payload byte. Defaults are zero/empty for fields omitted by a plan. The decoded
message and its copied payload are consumed inside the call and freed before
return; the benchmark returns a checksum, not an owning application message.

The smoke explicitly verifies input alias addresses and observes an input-byte
mutation through the alias while copied payload remains unchanged. It also checks
full→empty→sparse transitions, defaults, integer maxima, binary payloads including
NUL, malformed input rejection and unchanged external wire bytes.

## Interpretation limit

The comparison's existing C++ Protobuf baseline is **3.21.12**, whereas this upb
runtime is **v29.3** (the source repository's C++ version string is 5.29.3).
Differences combine implementation, versions, access APIs, allocation/reuse
strategies and ownership decisions. Do not attribute the measured ratio solely
to MiniTable versus Reflection, or call this generated upb's maximum throughput.
All performance execution must wait for the root agent's RUN coordination.
