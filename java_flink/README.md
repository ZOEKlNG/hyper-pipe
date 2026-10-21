# Java 21 / Flink 2.1.0 measurements

Prerequisites: Java 21 runtime, Python 3, bash, curl, tar, network access to Maven Central. `bash build.sh` automatically downloads pinned Maven 3.9.9 / ECJ 3.40.0 when absent and builds generated Protobuf 3.21.12 and Flink 2.1.0 classes. `MVN=/path/to/mvn bash build.sh` selects an existing Maven executable. The classpath version check rejects a mismatched Flink/Protobuf dependency. A JDK is not required because ECJ is used. The generated Java schema code is included; `event.proto` is its source. Run all CPU measurements serially on an otherwise idle host:

```sh
bash build.sh
CORPUS_DIR=/path/to/codec/corpus python3 run_all.py
```

For longer Flink data windows without rerunning the Java microbenchmark:

```sh
CORPUS_DIR=/path/to/codec/corpus FLINK_RECORDS=30000000 SKIP_CODEC=1 RESULT_LABEL=long_window python3 run_all.py
```

The corpus is exported by the companion C++ codec benchmark. Each file holds 4096 records, each framed by a four-byte little-endian byte length. Results go to `results/java_codec.csv` and `results/flink_local.csv`, with raw stdout/stderr per run. Long-window results are under `results/long_window/`; the delivered canonical copy is `results/flink_long.csv` (30M records/repetition), while `results/flink_short.csv` holds the exploratory 3M-record runs. Use long-window results for the main comparison. `run.sh` deliberately gives Java access to standard modules used by Flink on Java21.

Individual small validation runs (first execute one unreported warmup job) are possible with:

```sh
bash run.sh FlinkLocalBench /path/to/corpus fixed keyby 2 10000 1 64
```

Arguments are corpus directory, codec (`fixed` or `protobuf`), topology (`keyby` or `nokeyby`), parallelism, total records, measured repetitions, payload bytes. Read `notes.md` for the measurement boundaries. In particular, the Flink test emits projected tuples after decoding wire records; it does not carry the original payload bytes through the Flink shuffle.
