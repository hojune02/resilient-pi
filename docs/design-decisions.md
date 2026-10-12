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
