# Linux loopback transport benchmark

This benchmark compares complete C++ sender/receiver implementations on **one Linux host**, using the loopback interface. It does not measure a physical NIC, DPDK, AF_XDP, RDMA, Ethernet serialization, switch latency, or cross-machine latency.

## Build and run

With Debian/Ubuntu development packages installed:

```sh
g++ -std=c++20 -O3 -DNDEBUG -pthread transport.cpp -lzmq -o transport
./transport --out results --seconds 0.6 --repeats 5 --rounds 5000
python3 summarize.py results
```

In the original isolated workspace (the headers were extracted locally, without a root installation):

```sh
g++ -std=c++20 -O3 -DNDEBUG -pthread -I/workspace/work/deps/root/usr/include transport.cpp -Wl,-l:libzmq.so.5 -o transport
./transport --out results --seconds 0.6 --repeats 5 --rounds 5000
```

The program expects logical CPUs 1 and 2 to be available. It pins the sender application thread to CPU 1 and receiver application thread to CPU 2. ZeroMQ has an additional unpinned I/O thread. Process CPU time includes this thread; application thread CPU times alone do not. Run performance workloads serially to avoid interference. There is no CPU-frequency or host-neighbor isolation guarantee in a shared VM.

## Workload and measurements

- Body sizes are **64, 256, 1024 bytes total**, including an 8-byte monotonic sequence number. Remaining bytes are initialized to `x`. These are transport-only buffers, not Protobuf-encoded events.
- TCP adds a 4-byte network-order length to every record. UDP and ZeroMQ retain message boundaries without that application frame. Transport/IP/Ethernet overhead is excluded from `body_bytes`; loopback has no physical Ethernet link.
- `tcp_record` writes one framed record per send loop; `tcp_batch32` concatenates 32 complete framed records per write loop. Both set `TCP_NODELAY`, handle partial writes, and use buffered stream parsing that handles split/coalesced records.
- `udp_record` uses one `send`/`recv` per datagram. `udp_mmsg32` uses `sendmmsg`/`recvmmsg` with up to 32 independent datagrams; it does **not** put 32 messages into one large IP packet.
- `zmq_push_pull` uses a real libzmq PUSH/PULL connection over TCP, one record per ZeroMQ message. It sets send/receive HWM to 8192 messages, blocks on producer backpressure, and verifies delivered counts. HWM is not a hard bound for every byte buffered by all underlying layers.
- Kernel sender/receiver socket buffers request 2 MiB; CSV records the **actual** `getsockopt` values, which may be capped/doubled by Linux. These are settings in this experiment, not a recommended production queue size.
- For each body/mode pair: a separate 100 ms warmup run, then five independent runs with 600 ms of active sending. Connections and buffers are prepared before timing. CPU quotas, scheduling and host noise can affect these short trials; repeat on dedicated hardware for sizing.
- Reliable modes drain all successfully sent records before completion, check ordering and validate a sequence checksum. UDP drains until a receive timeout after the sender has stopped. UDP missing records are `sent - received`; they are not silently omitted from throughput. Sequence discontinuities count transitions/gaps, not the number of missing records.
- `received_mmsg_s` is delivered records divided by the span from first send-loop start to the later of sender completion and last receive. Drain time is included when delivery extends past sender completion. The idle UDP drain timeout is excluded from that span. Sender socket acceptance rate is not presented as delivered throughput.
- Sender/receiver CPU time uses `CLOCK_THREAD_CPUTIME_ID`; whole-process CPU uses `CLOCK_PROCESS_CPUTIME_ID`. Setup is outside timing; thread completion synchronization is included in process CPU measurement.

## RTT experiment

TCP, UDP and ZeroMQ REQ/REP use one outstanding request at a time, with 1000 warmup exchanges and 5000 measured exchanges per repeat. TCP uses length framing and `TCP_NODELAY`. No arrival rate is imposed. The sample is application-observed **round-trip** latency, including sender/echo code and system calls. It is neither a one-way latency measurement nor an under-load tail-latency guarantee. Dividing this RTT by two would not establish real one-way latency. UDP timeouts are reported; successful replies must match the request bytes.

Different protocols have different service and reliability semantics. These measurements compare the implementations supplied here under this workload; they do not establish a universal ranking of TCP, UDP and ZeroMQ.

## Paced open-loop UDP experiment

```sh
./transport --out results --paced --seconds 1 --repeats 3
```

This independent experiment sends **256-byte UDP datagrams** at target arrival schedules of 50k, 200k, 500k and 1M messages/s, with three repeats and a fixed **one-second planned window** per repeat. The sender busy-spins to scheduled `steady_clock` deadlines; it does not wait for receiver progress. This pacing consumes a CPU even at the lower offered rates. Both application threads remain on CPU 1/2. Each packet carries its original planned timestamp and timestamp immediately before the send call.

`offered_slots` is the number of arrivals in the desired schedule. If the sender cannot execute every slot before the one-second window ends, the remainder is `source_schedule_missed`. These are source-generator misses, not network drops. `attempted` includes all executed send calls; `accepted` counts successful nonblocking `MSG_DONTWAIT` socket submissions. EAGAIN and other send failures are counted separately. `accepted_minus_received` measures accepted datagrams missing at the receiver after drain, while `offered_minus_received` includes both generator misses/send errors and accepted-but-missing datagrams. The accounting identity is:

```
offered = source_schedule_missed + send_errors + accepted
accepted = accepted_minus_received + received
```

The program reports three different latency distributions:

- `arrival_minus_planned`: receiver timestamp minus the **original planned arrival schedule**, including generator lateness. This prevents a slow sender from silently shifting its intended arrival clock forward.
- `sender_lateness`: actual send timestamp minus planned timestamp, for attempted slots, including unsuccessful attempts.
- `arrival_minus_actual`: receiver timestamp minus actual send timestamp. This isolates the shorter socket/receiver path but must be read alongside generator lateness and misses.

**All receive-latency percentiles exclude dropped/unattempted datagrams.** Always read them together with offered/accepted/received counts and schedule misses. This test does not prove that the sender can sustain a configured rate simply because that target appears in the CSV. `max_behind_schedule_slots` records the largest schedule backlog observed before an attempted send. The fixed one-second source window discards pending arrivals rather than extending generation indefinitely.

Buffers for latency samples are preallocated and touched before the timing gate. Quantile sorting happens after the measured interval. There is no per-message codec work and no receiver application pause injected here; consumer-pause/overload modeling belongs to the separate simulation. `measurement_metadata.json` preserves start/end cgroup CPU statistics and exact command lines for the formal baseline and paced runs.

The reported `process_cpu_cores` is an approximate whole-process occupancy over the delivery span. The CPU timer also includes a small gate/join cost and CPU consumed while terminating a UDP timeout, whereas that final idle drain timeout is excluded from `delivery_seconds`. RTT timeout accounting includes warmup exchanges; all formal-run RTT timeout counts were zero. The UDP RTT harness aborts rather than silently matching a late response to a newer request, so a run with timeouts/reordering would require a more robust sequence-aware retry harness before use.

`zmq_send` is the copying baseline. This benchmark does not exercise application-owned messages via `zmq_msg_init_data`, Java, native buffer lifetime management, custom transport optimizations, or every tunable ZeroMQ option. TCP loopback batching can benefit from large loopback MTU and kernel aggregation/offload behavior; it must not be used to predict the same gain over physical Ethernet.
