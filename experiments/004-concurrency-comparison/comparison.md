# Day 6 — V1 vs V2 Concurrency Comparison

## Research question

Does replacing a single-threaded blocking TCP server with a bounded four-worker pool improve throughput, latency, resource predictability, and isolation between clients?

## Architectures

**V1:** A single thread performs `accept → recv → send → close`, with a one-second socket receive timeout.

**V2:** One acceptor thread dispatches accepted connections through a bounded FIFO queue of capacity 64 to four worker threads. Workers retain blocking I/O and the same receive timeout. Queue-full connections are closed without an HTTP response.

Both versions preserve per-request logging, although V2 additionally synchronizes logging with a mutex.

## Benchmark conditions

- Server: Raspberry Pi 4, Ubuntu Server ARM64
- Client: MacBook
- Transport: KAIST campus network, not direct Ethernet
- Compiler flags: C++20, `-O2`, warnings enabled, `-pthread`
- Workload: `wrk`, 15-second measurements, 5-second warm-ups
- Concurrency: 1, 10, 25, 50, 100
- Repetitions: 3 per architecture and concurrency level
- Measurement tool: `wrk --latency --timeout 2s`

## Results

| Connections | V1 req/s | V2 req/s | V2 change |
|---:|---:|---:|---:|
| 1 | 63.81 | 62.11 | -2.7% |
| 10 | 149.41 | 137.45 | -8.0% |
| 25 | 152.62 | 133.51 | -12.5% |
| 50 | 167.44 | 143.16 | -14.5% |
| 100 | 198.50 | 123.58 | -37.7% |

Values are means of three trials. V2 reported seven socket errors in one 100-connection trial.

## Observations

1. V2 did not improve throughput in the observed KAIST-network workload.
2. V2 degraded more substantially at higher concurrency.
3. Functional testing demonstrated that V2 can serve other clients while one worker is blocked.
4. V2 introduces fixed worker-thread overhead, queue synchronization, and serialized logging.
5. The primary cause of the throughput regression has not been isolated.

## Interpretation

The worker pool improves request isolation under blocking-client conditions, but those reliability benefits do not imply improved maximum throughput.

The observed regression may involve synchronization overhead, logging contention, queueing behavior, network conditions, or other factors. Current evidence does not identify one definitive cause.

## Limitations

- V1 and V2 were benchmarked in separate sequential batches.
- Campus network conditions may have varied between batches.
- Request-body correctness was not checked for every response.
- Worker-pool queue-full rejection counts were not captured during the throughput sweep.
- Process resource metrics still require comparison.
- Direct Ethernet validation remains pending.
## Overload Experiment: Bounded Queue Rejection

### Configuration

- Architecture: V2 bounded worker pool
- Worker threads: 4
- Application queue capacity: 64 accepted sockets
- Client workload: 100 TCP connections opened without sending HTTP requests
- Client hold duration: 2 seconds
- Receive timeout: 1 second

### Results

- Successfully established client-side TCP connections: 100/100
- Server-reported rejected connections: 32
- Server remained running long enough to log subsequent `/health` requests and complete graceful shutdown.

### Interpretation

The experiment exercised V2's queue-full admission-control path, demonstrating that the acceptor closes new connections when its bounded application queue is full.

The result does not mean 32 TCP handshakes failed. Client-side establishment and server-side application admission are distinct events.

The test confirms functional rejection behavior, but maximum queue occupancy, file-descriptor bounds, and exact rejection timing were not independently measured.

### Outstanding validation

- Confirm HTTP 200 after overload.
- Optionally record peak application queue depth and FD count.
- Repeat the overload experiment to assess consistency.

## Next validation

Repeat both architectures under the same direct-Ethernet setup when available. Separately investigate logging overhead and worker-count sensitivity.

## Preliminary decision

Retain V1 as the simpler throughput baseline. Retain V2 as the experimental bounded-concurrency architecture.

Do not claim V2 is a performance optimization until controlled measurements support that claim.
