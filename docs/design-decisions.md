## DDR-001: Investigating the Remote HTTP Throughput Bottleneck

### Context

The V1 server uses a single-threaded, blocking TCP request-processing loop with a one-second `SO_RCVTIMEO` per accepted client connection.

Day 4 testing on the KAIST network indicated throughput plateauing around 400–500 requests/second. Low process CPU utilization suggested that application CPU saturation was unlikely to be the sole limiting factor.

### Hypothesis

The network transport path contributes significantly to the observed remote throughput limit.

### Experiment

On October 10, 2026, the same Raspberry Pi server was benchmarked from the same MacBook over two network paths:

- Direct home LAN: `192.168.0.57:8080`
- Direct peer-to-peer Tailscale connection: Pi's Tailscale address, port 8080

Workload: `wrk -t2 -c25 -d15s --latency --timeout 2s`, with five-second warm-ups, ten-second cooldowns and three recorded trials per path.

Trial sequence: LAN → Tailscale → Tailscale → LAN → LAN → Tailscale.

### Observations

| Metric | LAN | Tailscale |
|---|---:|---:|
| Throughput mean (req/s) | 730.00 | 450.24 |
| Throughput standard deviation | 5.96 | 11.95 |
| Mean p50 latency (ms) | 15.45 | 25.61 |
| Mean run-level p99 latency (ms) | 31.89 | 47.78 |

LAN throughput exceeded Tailscale throughput by approximately 62.1%, relative to the Tailscale baseline.

Earlier process monitoring also identified substantial CPU consumption by `tailscaled`, logging services, and Wi-Fi driver work.

### Interpretation

The choice of network path materially affected the observed end-to-end HTTP performance in this controlled experiment.

The results support network-path overhead as an important factor. They do not isolate encryption cost from other differences in packet processing or routing.

Neither CPU saturation of the application process nor memory exhaustion has been demonstrated.

### Limitations

- Three trials per path provide limited statistical confidence.
- Wi-Fi conditions and background system activity were not fully controlled.
- Process CPU usage was not collected in a synchronized, matched comparison for all six trials.
- The measurements do not isolate application-level blocking behavior from network-path overhead.
- Run-level p99 averages are not pooled request-level p99 measurements.

### Decision

Preserve the current V1 implementation as the baseline for subsequent concurrency-architecture comparisons.

Investigate application logging overhead and bounded concurrent request handling in separate, versioned experiments rather than attributing all performance limitations to a single component.

## DDR-002: Bounded Worker Pool vs Single-Threaded Server

**Status:** Provisional — awaiting direct-Ethernet validation and overload measurements.

### Problem

A single blocking request-handling thread allows one slow client to delay unrelated clients. A receive timeout bounds individual receive waits but does not provide concurrent request execution.

### Alternatives evaluated

- V1: Single-threaded blocking server
- V2: Four-worker bounded thread pool with 64-entry application queue

### Evidence

V2 passed initial functional and slow-client-isolation tests. However, in a three-trial-per-configuration benchmark over the KAIST network, V2 delivered lower mean throughput than V1 across all five tested concurrency levels.

At 100 concurrent connections, V1 averaged 198.50 req/s, while V2 averaged 123.58 req/s. One V2 trial reported seven socket errors.

### Decision

Keep both architectures available for reproducible experimentation.

Prefer V1 where minimal implementation complexity and the observed throughput are primary concerns and slow-client interference is acceptable.

Prefer further development of V2 where client isolation, bounded concurrency, and explicit overload handling are requirements.

Neither implementation is considered production-ready.

### Trade-offs

V2 introduces worker scheduling, synchronization, memory overhead, and more complex shutdown and overload behavior. These costs must be justified by demonstrated reliability benefits and further performance measurements.

### Follow-up

- Direct-Ethernet architecture comparison
- Verified queue-capacity and rejection testing
- Logging overhead investigation
- Worker-count sensitivity analysis

---

## DDR-003: Reuse a PostgreSQL Connection with Explicit Failure Recovery

**Status:** Accepted for Day 7 prototype  
**Date:** 2026-10-13  
**Related experiment:** `005-postgres-persistence`

### Context

The Day 7 persistent HTTP server introduces a PostgreSQL dependency.

The application uses a single-threaded request-handling loop and executes database operations through `libpq`.

The primary requirements are:

- Preserve committed state across process restarts.
- Avoid unnecessary database connection establishment.
- Handle temporary PostgreSQL unavailability.
- Recover database connectivity without restarting the C++ application.
- Avoid unsafe retries of non-idempotent transactions.

### Problem

The initial implementation established one PostgreSQL connection at application startup and reused it across requests.

When PostgreSQL was stopped, database-backed requests returned HTTP 503 while the HTTP process remained alive.

After PostgreSQL restarted, the application continued returning HTTP 503 because its existing `PGconn` was no longer usable.

The PostgreSQL process recovered, but the dependent application did not.

### Alternatives Considered

#### A. Open a new connection for every HTTP request

Advantages:

- Each request obtains a fresh database connection.
- Failed connections are less likely to persist between requests.

Disadvantages:

- Connection establishment and authentication occur repeatedly.
- PostgreSQL must manage additional backend process churn.
- Resource usage can increase with request rate.

This alternative was not implemented or benchmarked.

#### B. Reuse one connection without recovery

Advantages:

- Minimal implementation complexity.
- Avoids repeated connection establishment.
- Appropriate when database availability is stable.

Disadvantages:

- Database restart can permanently invalidate the connection.
- Application recovery may require a process restart.

This was the initial implementation and demonstrated a real recovery failure.

#### C. Reuse one connection with explicit recovery

Advantages:

- Retains connection reuse.
- Allows the application to recover from observed PostgreSQL restarts.
- Fits the current single-threaded architecture.
- Requires minimal additional synchronization.

Disadvantages:

- Reconnection attempts can add request latency.
- Repeated failures may trigger repeated connection attempts.
- Ambiguous transaction outcomes still require special treatment.
- Does not support multiple simultaneous database operations.

This alternative was implemented and tested.

#### D. Bounded database connection pool

Advantages:

- Supports concurrent database operations.
- Bounds connection growth.
- Reduces connection-establishment churn.

Disadvantages:

- Requires acquisition, release, timeout and recovery policies.
- Adds synchronization and queueing complexity.
- Introduces additional connection-management failure cases.

This alternative is deferred until the HTTP architecture requires concurrent database access.

### Decision

Use one persistent PostgreSQL connection for the single-threaded Day 7 HTTP server.

Before starting a new database operation, inspect the connection state and establish a replacement connection if the existing connection is invalid.

Return HTTP 503 when database operations cannot complete.

Do not automatically repeat a transaction after it has begun if its commit outcome may be unknown.

Keep connection recovery and transaction retry as separate concerns.

### Evidence

The initial implementation reproduced the following failure:

1. PostgreSQL was stopped.
2. Database-backed HTTP operations returned HTTP 503.
3. The HTTP process remained running.
4. PostgreSQL restarted successfully.
5. Database-backed HTTP operations continued returning HTTP 503.

After implementing explicit connection recovery:

1. PostgreSQL was stopped.
2. `GET /counter` returned HTTP 503.
3. `GET /health` continued returning HTTP 200.
4. PostgreSQL restarted.
5. `GET /counter` returned HTTP 200 with the preserved counter value.
6. `POST /counter/increment` successfully committed another increment.

The application recovered without restarting its process.

### Correctness Considerations

A broken PostgreSQL connection can be replaced before executing a new operation.

However, if a connection is lost during COMMIT, the application may be unable to determine whether the transaction committed.

Automatically retrying the increment in that situation could duplicate a successful operation.

A more robust future design may use request-level idempotency keys and a database-backed deduplication mechanism.

### Resource Considerations

The observed application used one idle PostgreSQL backend connection between HTTP requests.

PostgreSQL reported a maximum of 100 connections, with three reserved for superusers.

This limit is not a target pool size.

A future multithreaded server should use a bounded connection count appropriate to measured workload demand and available database resources.

### Consequences

The application can now recover from the tested PostgreSQL restart sequence.

However, the current design retains:

- Blocking database queries
- One HTTP request-handling thread
- One database connection
- No reconnection backoff
- No transactional idempotency guarantee
- No application-level database readiness endpoint

### Follow-up Experiments

1. Measure query and COMMIT latency.
2. Measure connection recovery latency.
3. Test bounded reconnection backoff.
4. Investigate idempotent transaction requests.
5. Introduce a bounded connection pool if concurrent HTTP/database operations become necessary.
6. Add readiness and database availability metrics on Day 8.

### Supporting Evidence

- `experiments/005-postgres-persistence/methodology.md`
- `server/persistent_server.cpp`
- `database/schema.sql`
- `journal/day07.md`


---

## DDR-004: Lightweight In-Process Metrics with External Monitoring

**Status:** Accepted for Day 8 prototype  
**Date:** 2026-10-13  
**Related experiment:** `006-observability`

### Context

The Resilient Pi project initially relied on terminal inspection, HTTP tests and systemd logs to understand application behavior.

Day 7 introduced PostgreSQL persistence and demonstrated that the database could become unavailable while the HTTP application remained alive.

This created a need for telemetry that distinguishes:

- Service availability
- Request errors
- Request latency
- Database readiness
- Resource utilization
- Monitoring-system failures

The monitoring approach should remain understandable, reproducible and reasonably lightweight.

### Problem

A running process is not necessarily a functioning service.

For example, the C++ HTTP server can return HTTP 200 for `/health` while database-dependent endpoints fail.

CPU and memory measurements alone cannot identify these failures.

At the same time, adding observability introduces CPU, memory, storage and operational complexity.

The design must balance diagnostic usefulness against instrumentation and monitoring cost.

### Alternatives Considered

#### A. Logs only

Advantages:

- Simple to implement
- Useful for recording specific failure events
- Can preserve detailed diagnostic context

Disadvantages:

- Harder to calculate error rates and latency percentiles
- Requires parsing and aggregation
- Can generate significant I/O
- Risks accidental exposure of sensitive request data

Rejected as the sole observability mechanism.

#### B. Custom in-process metrics without external collection

Advantages:

- Minimal dependencies
- Low implementation complexity
- Can display current process state

Disadvantages:

- Historical data disappears when the process exits
- Harder to investigate previous outages
- No ready-made time-series query engine
- Requires custom visualization

Useful as a first instrumentation step but insufficient for historical monitoring.

#### C. Lightweight metrics with Prometheus and Grafana

Advantages:

- Standard metric exposition format
- Historical time-series collection
- Rate and percentile queries
- Reusable dashboard definitions
- Monitoring process lifecycle independent of application
- Compatible with external deployment later

Disadvantages:

- Additional processes and resource consumption
- Scrape-based sampling delay
- Possible telemetry gaps
- Requires configuration and security management
- Metrics cardinality must be controlled

Selected for Day 8.

#### D. OpenTelemetry traces and distributed tracing backend

Advantages:

- Can reveal execution paths across multiple services
- Supports correlation of distributed operations
- Useful for analyzing individual request journeys

Disadvantages:

- More instrumentation complexity
- Additional collection and storage components
- Limited immediate value for the current single-host architecture
- Does not replace basic error-rate and resource metrics

Deferred until a concrete tracing requirement emerges.

### Decision

Use lightweight in-process metrics implemented directly in C++.

Expose them through a localhost `/metrics` endpoint using Prometheus text exposition format.

Collect application metrics with Prometheus at five-second intervals.

Use Node Exporter for host CPU and memory measurements.

Visualize historical data using Grafana.

Keep Prometheus, Node Exporter and Grafana as independently managed systemd services.

Restrict monitoring network listeners to localhost and use SSH tunneling for remote dashboard access.

Avoid high-cardinality and sensitive labels.

### Metric Design

The application exports:

- HTTP response-attempt count
- HTTP error-response count
- HTTP send-failure count
- HTTP request duration histogram
- Active accepted connections
- Process uptime
- Last-observed database connection state
- Database-operation duration histogram

Node Exporter provides host CPU, memory and load metrics.

Prometheus provides scrape-health metrics.

The dashboard contains eight panels covering application throughput, errors, latency, database state, active connections, CPU and memory.

### Evidence

#### Application instrumentation

A synthetic histogram smoke test correctly recorded three observations.

Real HTTP traffic changed request counters, error counters and latency histograms as expected.

#### Database outage

Stopping PostgreSQL produced:

```text
GET /health  -> HTTP 200
GET /ready   -> HTTP 503
GET /counter -> HTTP 503
```

The database connection gauge changed to zero.

After restarting PostgreSQL, database-backed requests returned HTTP 200 and the gauge returned to one.

The HTTP error counter retained the two recorded failures.

#### Monitoring failure isolation

Prometheus was stopped while the application continued running.

Observed:

```text
GET /health  -> HTTP 200
GET /counter -> HTTP 200, 16
```

After restarting Prometheus:

```promql
up{job="resilient-pi"}
```

returned one.

This demonstrated that Prometheus is not required for normal application request handling.

### Performance and Resource Evidence

#### Application memory

| Implementation | Observed RSS |
|---|---:|
| Persistent server | 7848 KiB |
| Observable server | 7952 KiB |

The observable server's RSS was 104 KiB higher in a single snapshot.

This is not an isolated measurement of instrumentation memory overhead.

#### Monitoring memory

Observed systemd cgroup memory:

| Component | Approximate memory |
|---|---:|
| Prometheus | 65.76 MiB |
| Node Exporter | 9.64 MiB |
| Grafana | 458.15 MiB |
| Combined | 533.55 MiB |

The monitoring stack has a substantially larger memory footprint than the application's observed instrumentation difference.

#### Latency benchmark

Six alternating rounds of 100 sequential read-only requests per server produced:

```text
Persistent server mean: 2.175 ms
Observable server mean: 2.022 ms
```

The result does not establish a performance improvement caused by instrumentation.

No clear instrumentation-related latency regression was resolved under the tested workload.

### Consequences

Benefits:

1. Application failures are observable through historical metrics.
2. Database readiness is distinguishable from HTTP process liveness.
3. Request latency distributions can be analyzed.
4. Host resource measurements are available.
5. Dashboards can be recreated from version-controlled JSON.
6. Monitoring services can fail independently of application request handling.

Costs:

1. Additional memory and CPU consumption
2. Additional operational services
3. Time-series storage on the Pi
4. Scrape traffic handled by the application
5. Monitoring configuration and network-security requirements

### Known Limitations

- The observable server is single-threaded.
- `/metrics` uses the application's request-handling loop.
- Scrapes contribute to request counters and active-connections measurements.
- HTTP request counts and latency observations can have different populations.
- Database availability is based on the last check rather than continuous probing.
- Percentiles derived from histograms are approximate.
- Sparse traffic can make short-window quantile estimates unstable.
- Process-specific CPU usage is not exported.
- Monitoring-stack overhead has not been measured under a sustained controlled workload.
- The dashboard screenshot does not capture the earlier PostgreSQL outage interval.

### Future Reconsideration Criteria

Revisit this decision if:

1. The deployment moves to a smaller memory-constrained device.
2. Grafana and Prometheus resource consumption affects application behavior.
3. The server becomes multithreaded and metrics require synchronization.
4. Multiple services require request-level tracing.
5. Operational requirements demand alerting, longer retention or remote monitoring.
6. Metric cardinality grows beyond predictable bounded dimensions.

For a constrained edge deployment, retain local instrumentation while considering a separate monitoring host.

### Supporting Files

```text
server/observable_server.cpp
server/metrics.hpp

experiments/006-observability/methodology.md
experiments/006-observability/metrics.md
experiments/006-observability/compare_overhead.py

observability/grafana/resilient-pi-dashboard.json

results/raw/day08/
results/figures/day08-observability-dashboard.png

journal/day08.md
```
---

## DDR-005: Investigate Worker-Pool Throughput and High-Concurrency Socket Failures

**Status:** Investigation accepted; optimization pending  
**Date:** 2026-10-13  
**Related experiments:** Day 6 concurrency benchmark, direct-Ethernet benchmark  
**Affected implementation:** `server/worker_pool.cpp`

### 1. Context

Resilient Pi implements two HTTP server architectures:

- **V1:** Single-threaded, blocking `accept()` → `recv()` → `send()` → `close()`.
- **V2:** One acceptor thread, four worker threads, a mutex-protected bounded queue of 64 pending connections, and a condition variable.

V2 was introduced to improve concurrency and prevent a single slow client from blocking every other request.

The original benchmark suggested that V2 had lower throughput than V1. A new direct-Ethernet experiment was performed to reduce network-path variability.

The client was Arch Linux, connected directly to the Raspberry Pi at `10.42.0.2` over a 100 Mb/s full-duplex Ethernet link.

Both implementations were tested using `wrk`, with five-second warmups and three 15-second measured runs per concurrency level.

Routine server output was redirected to `/dev/null` for both implementations.

### 2. Benchmark Results

| Concurrent connections | V1 mean req/s | V2 mean req/s | V2 relative difference |
|---|---:|---:|---:|
| 1 | 2,053 | 1,281 | -37.6% |
| 10 | 2,369 | 1,905 | -19.6% |
| 25 | 2,340 | 2,090 | -10.7% |
| 50 | 2,923 | 2,627 | -10.1% |
| 100 | 3,831 | 3,833 | Approximately equal |

V1 delivered higher mean throughput at concurrency levels 1–50.

At concurrency 100, reported throughput was approximately equal, but V2 produced socket read errors.

Raw V2 concurrency-100 results:

| Run | Requests | Socket read errors |
|---|---:|---:|
| 1 | 56,086 | 1,682 |
| 2 | 56,142 | 1,047 |
| 3 | 61,077 | 1,499 |
| Total | 173,305 | 4,228 |

V1 reported no socket errors in its measured runs.

The initial benchmark CSV parser incorrectly recorded zero errors for V2. The raw `wrk` logs preserve the actual error counts.

V2 also showed substantially more run-to-run throughput variability at several concurrency levels.

### 3. Problem Statement

Adding worker threads did not reliably improve throughput.

The worker-pool implementation introduces synchronization, scheduling, and queue-management costs absent from V1.

The observed high-concurrency socket failures also raise questions about V2's overload behavior.

The engineering problem is therefore:

**Under which workloads does the worker pool improve request handling, and what implementation or operating-system behavior causes its throughput and reliability limitations?**

### 4. Working Hypotheses

#### H1 — Synchronization overhead

Mutex acquisition, queue operations, condition-variable signaling, and worker scheduling may increase request-processing cost.

This overhead may dominate for very short requests.

**Validation:** Measure queue-lock wait time, enqueue/dequeue duration, and context-switch counts.

#### H2 — Queue saturation and rejected connections

The 64-entry application queue may fill when incoming connections arrive faster than workers can process them.

Rejected connections may contribute to the observed socket read errors.

**Validation:** Instrument queue depth, accepted connections, rejected connections, and busy workers. Correlate these values with client-side failures.

#### H3 — Blocking socket operations

Each worker performs blocking socket operations. Workers can become unavailable while waiting for clients.

**Validation:** Measure time spent waiting in `recv()` versus performing useful request processing.

#### H4 — Logging and synchronization contention

Both servers log request data. V2 also synchronizes logging between worker threads.

Redirecting stdout does not eliminate formatting, locking, or all logging-related execution costs.

**Validation:** Compare equivalent implementations with per-request logging disabled at compile time.

#### H5 — Client or kernel bottleneck

The benchmark includes repeated TCP connection establishment and teardown because the servers close connections after each response.

Client scheduling, TCP socket management, network interrupts, and connection churn may influence measurements.

**Validation:** Inspect client/server CPU, context switches, TCP statistics, and interface counters during controlled trials.

### 5. Decision

Do not assume that four workers are inherently faster than a single-threaded server.

Do not increase the worker count, queue capacity, or timeout values without measurements.

Preserve the existing V1 and V2 implementations as baseline references.

Investigate V2 through incremental instrumentation and controlled experiments.

Prioritize identifying the cause of high-concurrency socket read errors before optimizing peak throughput.

### 6. Investigation Plan

**Phase 1 — Establish correctness**

- Fix the benchmark's socket-error parser.
- Preserve all raw benchmark results.
- Instrument connection admission and rejection.
- Measure peak queue depth and busy workers.
- Reproduce the V2 concurrency-100 read errors.
- Determine whether errors correlate with queue rejection, socket failures, or another source.

**Phase 2 — Measure scheduling and synchronization**

- Measure enqueue and dequeue operations.
- Measure time spent waiting for a worker.
- Record process CPU utilization and voluntary/involuntary context switches.
- Determine whether workers are CPU-bound, blocked, or waiting on synchronization.

**Phase 3 — Isolate logging overhead**

- Build equivalent V1/V2 variants with routine per-request logging disabled.
- Keep compiler flags, request behavior, and test conditions consistent.
- Compare results against the original binaries.

**Phase 4 — Test slow-client isolation**

- Hold one or more TCP connections open without sending requests.
- Send ordinary HTTP requests from another client.
- Compare successful response rates and tail latency.
- Identify conditions in which V2 maintains responsiveness while V1 stalls.

**Phase 5 — Optimize and retest**

Only after identifying a bottleneck, consider changes such as:

- Reducing lock scope
- Changing queue admission behavior
- Improving socket error handling
- Separating logging from request processing
- Adjusting worker count based on measured workloads
- Introducing nonblocking I/O if requirements justify it

Each optimization must be evaluated against the unchanged baseline.

### 7. Success Criteria

V2 should not be considered an improvement solely because it uses multiple threads.

A successful revised design should demonstrate:

1. Correct HTTP response handling.
2. Predictable behavior under overload.
3. No unexplained socket failures under tested operating conditions.
4. Measurable benefits under workloads that require concurrency.
5. Acceptable request latency and resource consumption.
6. Reproducible results across multiple trials.

If V2 offers better slow-client isolation but lower throughput for tiny requests, document that as an intentional engineering trade-off rather than an optimization failure.

### 8. Known Experimental Limitations

- The link operated at 100 Mb/s rather than Gigabit speed.
- V1 and V2 were benchmarked in separate blocks, creating possible time-order bias.
- Only three repetitions were collected per configuration.
- CPU, lock contention, and queue depth were not measured during the original direct-Ethernet trials.
- `wrk` throughput does not capture all connection-level failure events.
- The workload used very small responses and repeated TCP connection establishment.
- Redirecting logs to `/dev/null` did not remove in-process logging overhead.
- The benchmark has not yet established a root cause for V2's lower throughput.

### 9. Consequences

The project will evaluate concurrency as a workload-dependent architectural trade-off.

Future performance claims will distinguish:

- Peak throughput
- Tail latency
- Error and rejection behavior
- Resource efficiency
- Availability under slow or blocking clients

A worker-pool implementation will be considered useful when evidence shows that its concurrency benefits justify its additional complexity.

### 10. Supporting Artifacts

- `server/main.cpp`
- `server/worker_pool.cpp`
- `build/day06/v1`
- `build/day06/v2`
- Direct-Ethernet `wrk` raw outputs
- Direct-Ethernet benchmark CSV files
- Day 6 concurrency experiment documentation

**Follow-up status:** Root-cause investigation remains open.
### Follow-up Evidence — Isolated V2 Concurrency-100 Diagnostics

**Date:** 2026-10-13

Two additional direct-Ethernet V2 tests were performed at 100 concurrent connections using two `wrk` threads and a 15-second duration.

| Metric | Diagnostic 1 | Diagnostic 2 |
|---|---:|---:|
| Requests/sec | 5,437.89 | 5,405.64 |
| Completed requests | 81,597 | 81,125 |
| Median latency | 3.62 ms | 2.51 ms |
| p99 latency | 438.74 ms | 453.03 ms |
| Socket read errors | 0 | 0 |
| Timeouts | 1 | 1 |
| Server-side rejections | Not collected | 0 |

The second experiment preserved the server's shutdown diagnostic output while redirecting routine per-request logging to `/dev/null`.

The server terminated gracefully and reported:

```text
Rejected connections: 0
```

**Finding:** The application queue did not reject connections during the second diagnostic run. Queue rejection therefore cannot explain the observed socket errors in that particular run.

However, the earlier concurrency-100 tests reported 4,228 socket read errors across three runs. Their server-side rejection counts were not collected, so queue overflow remains an unverified explanation for those earlier failures.

The diagnostic experiments also produced substantially higher throughput than the earlier benchmark runs. Potential confounding factors include test order, process lifetime, background workload and TCP connection state.

**Decision:** Do not modify worker count or queue capacity yet. Prioritize controlled experiments that correlate client errors, server rejections, process CPU, and context-switch activity.

The current evidence establishes workload-dependent and experiment-dependent behavior, not a confirmed worker-pool bottleneck.

### Follow-up Evidence — Queue Rejection Correlates with Client Read Errors

**Date:** 2026-10-13

During three consecutive 100-connection `wrk` trials, V2 reported 12,181 socket read errors.

After graceful shutdown of the same benchmark server process, V2 reported:

```text
Rejected connections: 12216
```

The difference was 35 events.

This is strong evidence that connections rejected by V2's bounded 64-slot application queue account for most of the observed client-side read errors.

The counts are not expected to match exactly because the server's rejection counter includes the full process lifetime, including warmup periods, while client error counts cover the three measured benchmark intervals.

A separate isolated V2 diagnostic run reported zero rejected connections and zero read errors, further supporting the relationship between queue rejection and client-visible failures.

**Conclusion:** Queue-full admission rejection is a strongly supported contributor to V2's high-concurrency read errors.

**Remaining questions:**

- Why does the queue reach capacity under some runs but not others?
- How often are all four workers simultaneously occupied?
- What is the distribution of queue waiting time?
- Does per-request logging cause additional worker contention?
- Would changing admission behavior reduce errors without significantly increasing tail latency?

**Decision:** Preserve the original V2 baseline. Instrument worker utilization and queue behavior before modifying the worker count, capacity or overload policy.

A separate cooldown experiment on V1 showed throughput falling from 3,095 to 1,775 requests/sec across immediate consecutive runs and recovering to 2,763 requests/sec after 120 seconds. This supports a transient performance condition but does not identify its cause.

The queue-rejection problem and repeated-test performance degradation will be investigated independently.
---

## DDR-006: Treat CPU Saturation as an Admission-Control and Scheduling Problem

**Status:** Accepted for experimental interpretation  
**Date:** 2026-10-14  
**Related experiment:** `007-cpu-saturation`

### Context

The Resilient Pi HTTP server runs on a four-core Raspberry Pi.

Previous benchmarks showed that throughput and tail latency can vary substantially under sustained TCP connection churn.

Experiment 007 investigated whether CPU resource pressure contributes to degraded HTTP performance.

### Evidence

CPU pressure was generated using 0–4 `stress-ng` workers.

A synchronized four-worker stress experiment showed:

- Host CPU utilization: 99.9%
- HTTP throughput: 2092.75 requests/sec
- Initial baseline throughput: 2753.95 requests/sec
- Baseline p50 latency: 5.48 ms
- Saturated p50 latency: 11.02 ms
- Runnable-task counts frequently exceeding the four available CPU cores

The firmware throttling query returned `0x0`, and observed CPU frequency remained stable during the stressed samples.

### Decision

Treat CPU saturation as a potential source of scheduler contention and request latency rather than assuming degraded throughput necessarily indicates thermal throttling.

Do not optimize worker counts or modify CPU frequency based solely on aggregate CPU utilization.

Measure:

1. Application throughput and error rate
2. p50/p95/p99 latency
3. Per-core CPU utilization
4. Runnable-task pressure
5. Process CPU usage
6. Temperature and frequency

Future overload control should consider bounded admission, request timeouts, and load shedding before accepting unbounded demand.

### Trade-offs

Admission control can protect latency and memory resources but may reject requests.

Worker pools can isolate blocking operations but introduce coordination and scheduling costs.

CPU limits can improve resource isolation but may reduce available application throughput.

### Limitations

The experiment did not independently establish exact scheduling delays or quantify the effect of the cooling fan.

The observed absence of throttling flags does not establish that throttling can never occur under longer or hotter workloads.

### Follow-up

Evaluate bounded admission and CPU resource isolation under sustained load, preserving a reproducible baseline before each change.
