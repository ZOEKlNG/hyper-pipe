# C++ codec and compression benchmarks

## Build and run

Requirements: C++20 compiler, protoc + matching Protobuf runtime/development files, FlatBuffers flatc + headers, nlohmann/json 3.12.0, liblz4 and libzstd. `build.sh` uses `DEP_ROOT` (default `/workspace/work/deps/root`) and the system zstd library. Exact dependency versions and source download instructions are part of the parent benchmark bundle.

```sh
./build.sh
mkdir -p corpus
./bench --export-only              # correctness check + shared C++/Java corpus
./bench > codec_raw.csv 2> codec.log
./compression > compression_raw.csv 2> compression.log
python3 summarize.py /workspace/work/results
```

Run only one benchmark at a time. These executables request affinity to CPU0 and print whether affinity succeeded. The code runs on x86-64 little-endian in this version; the fixed layout explicitly rejects a big-endian host at build time. This is a microbenchmark, not a production protocol implementation.

## Common data and measurement

4096 changing events per corpus, deterministic PRNG, payload lengths 0/64/256/1024 bytes, random payload or repetitive synthetic log text. The fixed header is 64 bytes: offsets 0/8/16/24/32 contain five u64 (`id`, `seq`, `timestamp`, `value0`, `value1`); offsets 40/44/48/52 four u32 (`producer`, `stream`, `type`, `flags`); offset56 u16 `header_len`, offset58 u16 `version`, offset60 u32 `payload_len`. All numbers are little-endian. The payload follows at offset64. This implementation uses `memcpy` to/from a properly aligned local header, avoiding undefined unaligned `reinterpret_cast` reads.

The same logical data is encoded by official generated Protobuf 3.21.12 and FlatBuffers23.5.26 code. Protobuf compares default varint integer fields, fixed64/fixed32 fields, reused objects, and fresh objects. JSON uses nlohmann/json3.12.0 DOM plus hex encoding for the arbitrary binary payload. It is not a simdjson/RapidJSON benchmark, and its hex payload is twice the binary payload length. The fixed format includes 8 bytes of format metadata not present in the protobuf message; a production protobuf envelope could add these separately.

Codec operations are measured independently: encode, decode_all, decode_few, and encode+decode roundtrip. `decode_all` consumes every numeric field plus payload length and its first/last bytes; it does not scan or transform the entire payload. `decode_few` consumes only id/seq/type, although protobuf/JSON still parse the complete serialized object. Every pre-encoded event is validated against an independently calculated checksum before timing. `escape()` memory barriers make encoded output observable and prevent the compiler from eliminating unused payload writes. Public codec methods are noinline; virtual dispatch is included.

Warmup: 8192 operations per timed repetition. The timed batch doubles from4096 operations until at least20ms elapsed, with5 repetitions. A repetition's ns_per_op is its last batch wall time divided by operations. Report the median/min/max of the five batch means; these are not single-message latency percentiles. Adaptive calibration before the final batch also warms caches. Source events, wire strings, allocator capacity, and codec objects remain in a hot reusable working set. CPU cache misses, NUMA, contention, network I/O, JVM work, queueing, schema rollout, and consumer business processing are outside this benchmark.

`fixed64_offset_trusted` assumes a valid frame. `_checked` additionally checks minimum header size, schema version, header length, and payload bounds. It has not been hardened/fuzzed into a production decoder. `flatbuffers_trusted` does not run a verifier; `flatbuffers_verified` does. Protobuf and JSON parsers validate their own wire formats. Trusted vs validated rows must be distinguished. Fixed offset fields use no generated schema evolution machinery. FlatBuffers encode includes its reused builder construction and one copy into an owning std::string, whereas protobuf serializes to that output string; a builder-owned send buffer can remove this particular FlatBuffers copy in another API design.

Corpora in `corpus/` contain4096 records each, **u32 little-endian wire length followed by wire bytes** for both fixed and protobuf, enabling Java to process exactly the same events. `--export-only` performs validation and writes those corpora without timing.

## Compression benchmark

This is a distinct synthetic workload, not the codec corpus: records of128/320/1088 bytes (64-byte randomized header plus64/256/1024-byte payload), 256 changing blocks; batch size1 or32 records. The payload is random or repetitive log-like text. Header bytes are deliberately incompressible; this is a conservative compression ratio model for numeric headers, not a measurement of the exact codec data above. LZ4 default and zstd level1 are measured separately for compression and decompression; memcpy is measured with a noinline wrapper and an output memory barrier. Buffers are reused, compressed/decompressed bytes validated before timing, and every timed invocation remains an actual library call. All5 repetition values are batch means. `ns_per_record` divides batch time by the number of records, while `output_bytes_batch_mean` is bytes per entire batch. Use their ratio to calculate wire savings. Tiny records and random bytes may expand. Compression calls include the public stateless API's internal overhead, so reusable compression contexts may perform better.

## Supplementary API-design controls

`flat_direct.cpp` repeats encoding with the same reused FlatBufferBuilder while retaining its buffer, avoiding the final copy to std::string; it also measures build+immediate field access from that builder-owned buffer. It changes only this lifetime/API choice, not the payload creation inside the builder. A production asynchronous network sender cannot reuse this builder until the transport has finished with the buffer. These rows appear separately in `flat_direct_raw.csv` and `flat_direct_summary.csv`.

`compression_reuse.cpp` repeats the exact synthetic compression matrix with one reused `ZSTD_CCtx` and `ZSTD_DCtx`, using `ZSTD_compressCCtx`/`ZSTD_decompressDCtx`. Compare these with the stateless zstd rows before drawing conclusions about the cost of compression on tiny messages. Results appear in `compression_reuse_raw.csv` and `_summary.csv`. The batch32 result is CPU/ratio per record for a contiguous memory block; it does not imply sending the whole block in one UDP datagram. A 32×128-byte block exceeds a1500-byte network MTU after headers and needs a larger MTU or transport segmentation. In the architecture, use multiple MTU-sized datagrams or DPDK/sendmmsg bursts rather than relying on IP fragmentation.

`./run.sh` runs every microbenchmark sequentially and records cgroup CPU counters around each phase. The five batch means should not be combined into individual-message p99 latency.

Review note: the adaptive loop now stops at its maximum count only **after timing that actual count**, preventing a denominator error on exceptionally fast operations. The original loop had a latent cap-boundary issue; an audit of all1780 reported raw rows found zero rows reaching that boundary (`iterations >= 2^24`), so existing measurements require no correction. `flat_direct.cpp` includes the corrected common codec timing function.
