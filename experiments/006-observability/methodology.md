
# Experiment 006 — Observability and Monitoring Failure Isolation

**Project:** Resilient Pi  
**Experiment date:** 2026-10-13  
**Roadmap milestone:** Day 8 — Observability  
**Environment:** Raspberry Pi 4B, Ubuntu Server 24.04 ARM64

## 1. Objective

Build an observability system for a C++ HTTP server backed by PostgreSQL.

The investigation focuses on five engineering questions:

1. Can we observe application behavior without directly inspecting process logs?
2. Can telemetry distinguish application liveness from database readiness?
3. Can metrics reveal failures that CPU and memory utilization do not explain?
4. How much overhead does instrumentation and monitoring introduce?
5. Can the monitoring infrastructure fail without taking down the application?

The experiment prioritizes reproducible evidence over dashboard appearance.

## 2. Experimental Environment

| Component | Configuration |
|---|---|
| Hardware | Raspberry Pi 4 Model B |
| CPU | Four ARM cores |
| RAM | Approximately 8 GB, 7.6 GiB reported |
| Operating system | Ubuntu Server 24.04 ARM64 |
| Database | PostgreSQL 16 |
| Application language | C++20 |
| Database interface | libpq |
| Original persistent server | 127.0.0.1:8081 |
| Observable server | 127.0.0.1:8082 |
| Prometheus | 127.0.0.1:9090 |
| Node Exporter | 127.0.0.1:9100 |
| Grafana | 127.0.0.1:3000 |

The original systemd-managed V1 server on port 8080 was left unchanged.

The Day 7 PostgreSQL-backed server was copied to `server/observable_server.cpp` so the original implementation remained available for comparison.

All observability components were configured for loopback access, avoiding direct exposure through the campus-facing network interface.

Grafana was accessed remotely through an SSH tunnel.

## 3. Observability Architecture

```text
                    Raspberry Pi
  ┌──────────────────────────────────────────┐
  │                                          │
  │  HTTP Client                             │
  │      │                                   │
  │      ▼                                   │
  │  C++ Observable Server                   │
  │  127.0.0.1:8082                          │
  │      │                                   │
  │      ├── GET /health                     │
  │      ├── GET /ready                      │
  │      ├── GET /counter                    │
  │      ├── POST /counter/increment         │
  │      └── GET /metrics                    │
  │      │                │                 │
  │      ▼                │                 │
  │  PostgreSQL           │                 │
  │  Local Unix socket    │                 │
  │                       ▼                 │
  │                  Prometheus             │
  │                  :9090                  │
  │                       ▲                 │
  │                       │                 │
  │                  Node Exporter          │
  │                  :9100                  │
  │                       │                 │
  │                       ▼                 │
  │                    Grafana              │
  │                    :3000                │
  │                                          │
  └──────────────────────────────────────────┘
                         │
                         │ SSH tunnel
                         ▼
                  MacBook browser
```

Prometheus scrapes the C++ application's `/metrics` endpoint and Node Exporter's `/metrics` endpoint.

Grafana queries Prometheus rather than querying the application directly.

The monitoring components have independent process lifecycles from the application.

## 4. Application Instrumentation

A lightweight metrics implementation was added in:

- `server/metrics.hpp`
- `server/observable_server.cpp`

The instrumentation uses:

- Monotonic clocks for elapsed duration
- Integer counters for cumulative events
- Gauges for current or last-observed state
- Fixed-bucket histograms for latency distributions
- Prometheus text exposition format

The observable application is single-threaded, so the current metrics structure does not require synchronization for concurrent modifications.

This assumption must be revisited if the implementation is integrated into the V2 worker-pool server.

### Histogram implementation

The latency bucket boundaries, in seconds, are:

```text
0.001
0.0025
0.005
0.01
0.025
0.05
0.1
0.25
0.5
1
2.5
5
+Inf
```

Buckets are cumulative.

Each observation increments every bucket whose upper boundary is greater than or equal to the observed duration.

The exporter additionally reports `_sum` and `_count`.

## 5. Metrics Export Verification

### Procedure

1. Compile the metrics smoke test.
2. Record known artificial durations.
3. Inspect the exported histogram buckets.
4. Integrate the metrics implementation with the real HTTP server.
5. Generate successful and unsuccessful HTTP requests.
6. Query `/metrics`.

### Initial smoke test

Three synthetic HTTP durations were recorded:

- 3 ms
- 12 ms
- 150 ms

Observed:

```text
resilient_http_request_duration_seconds_sum 0.165
resilient_http_request_duration_seconds_count 3
```

The cumulative histogram buckets matched the known observations.

These were test inputs, not measured server latencies.

### Real HTTP validation

Requests:

```text
GET /health          -> HTTP 200
GET /counter         -> HTTP 200, value 16
GET /does-not-exist  -> HTTP 404
```

Observed metrics:

```text
resilient_http_requests_total 3
resilient_http_errors_total 1
resilient_http_send_failures_total 0
resilient_http_active_connections 1
resilient_http_request_duration_seconds_count 3
```

The active-connection gauge reported 1 while `/metrics` was being served because the scrape itself was an accepted connection.

A subsequent scrape increased the request count to 4.

### Accounting limitations

- The HTTP request counter counts response attempts.
- The latency histogram measures accepted-connection handling time, including some connections that may not generate responses.
- The request counter and latency histogram therefore do not necessarily have identical populations.
- A metrics response describes a snapshot taken before its own handling has completed.
- A successful socket send does not prove receipt by the remote client.
- The server has an intentionally limited HTTP request parser.

## 6. Database Readiness and Latency

The observable server introduces:

```text
GET /health
GET /ready
```

`/health` answers whether the HTTP application is responsive.

`/ready` performs a real PostgreSQL query and tests whether the database dependency is usable.

Database-backed operations are instrumented using a monotonic clock.

The metric is named:

```text
resilient_db_operation_duration_seconds
```

It includes application-side database operations and potential reconnection attempts.

It does not represent SQL execution time alone.

### Healthy-state observation

```text
GET /health  -> HTTP 200, OK
GET /ready   -> HTTP 200, READY
GET /counter -> HTTP 200, 16
```

Observed:

```text
resilient_db_connection_up 1
resilient_db_operation_duration_seconds_sum 0.002226428
resilient_db_operation_duration_seconds_count 1
```

The single operation duration was approximately 2.226 ms.

One observation is insufficient to characterize database latency statistically.

## 7. Experiment A — PostgreSQL Failure Injection

### Hypothesis

A database outage should produce observable application errors without causing the HTTP process to stop.

The monitoring system should distinguish dependency readiness from process liveness.

### Procedure

1. Save a baseline metrics snapshot.
2. Stop `postgresql@16-main`.
3. Request `/health`, `/ready` and `/counter`.
4. Save metrics during the outage.
5. Restart PostgreSQL without restarting the C++ application.
6. Repeat database-dependent requests.
7. Save metrics after recovery.

### Observations

During the outage:

```text
GET /health  -> HTTP 200
GET /ready   -> HTTP 503
GET /counter -> HTTP 503
```

Metrics:

```text
resilient_http_requests_total 8
resilient_http_errors_total 2
resilient_db_connection_up 0
resilient_db_operation_duration_seconds_sum 0.002503572
resilient_db_operation_duration_seconds_count 2
```

After PostgreSQL restarted:

```text
GET /ready   -> HTTP 200
GET /counter -> HTTP 200, 16
```

Metrics:

```text
resilient_http_requests_total 11
resilient_http_errors_total 2
resilient_db_connection_up 1
resilient_db_operation_duration_seconds_count 3
```

### Findings

- The HTTP application remained alive during PostgreSQL unavailability.
- Database-dependent endpoints exposed the dependency failure through HTTP 503.
- The database availability gauge changed from 1 to 0 and returned to 1 after recovery.
- The HTTP error counter retained the two failures after recovery.
- Database reconnection succeeded without restarting the HTTP application.

### Limitations

The database availability gauge represents the last observed result of a database operation or readiness check.

It is not an independent continuous probe.

The fault injection was conducted before the full monitoring stack was deployed; it therefore does not demonstrate the outage being captured historically in Grafana.

## 8. Prometheus Integration

Prometheus was installed as an independent systemd-managed process.

Scrape configuration included:

```yaml
scrape_configs:
  - job_name: resilient-pi
    scrape_interval: 5s
    static_configs:
      - targets: ["127.0.0.1:8082"]

  - job_name: node
    scrape_interval: 5s
    static_configs:
      - targets: ["127.0.0.1:9100"]
```

These represent the relevant scrape jobs, not the entire installed configuration.

The configuration was validated with `promtool`.

Application scraping was verified with:

```promql
up{job="resilient-pi"}
```

Observed result:

```text
1
```

Node Exporter scraping was verified with:

```promql
up{job="node"}
```

Observed result:

```text
1
```

A successful scrape indicates that Prometheus obtained metrics from the exporter.

It does not prove that the application's database dependency is ready.

## 9. Node Exporter Integration

Node Exporter provided host-level metrics including:

```text
node_cpu_seconds_total
node_memory_MemAvailable_bytes
node_memory_MemTotal_bytes
node_load1
```

Example observations:

```text
Available memory: 7096717312 bytes
Total memory:     8181051392 bytes
1-minute load:    0.41
```

Four CPU idle-time series were returned, corresponding to four CPU cores.

Host CPU utilization was derived from the rate of cumulative CPU idle time.

Node Exporter does not directly measure the memory consumption of a particular C++ application process.

Process-level RSS was inspected separately using `/proc`.

## 10. Grafana Dashboard

Grafana was installed as a separate service and connected to Prometheus.

The dashboard was exported as JSON:

```text
observability/grafana/resilient-pi-dashboard.json
```

It contains eight panels:

1. HTTP throughput
2. HTTP error rate
3. HTTP p95 latency
4. Database availability
5. Database operation p95 latency
6. Active HTTP connections
7. Host CPU utilization
8. Host available memory

Real traffic was generated using 100 successful `/counter` requests and 10 requests to an unknown endpoint.

The dashboard displayed application and host metrics.

### Screenshot observations

The screenshot showed:

- A visible increase in HTTP throughput
- A visible increase in HTTP errors
- HTTP p95 latency generally around 2–2.5 ms
- Database availability remaining at 1
- Active accepted connections remaining at 1 during scrapes
- Changes in host CPU utilization
- Host available memory around 6 GiB

The screenshot does not show a PostgreSQL failure interval.

### Evidence

Expected repository destination:

```text
results/figures/day08-observability-dashboard.png
```

The screenshot should be copied from the actual Grafana session into this location.

## 11. Experiment B — Process Memory Comparison

### Procedure

Inspect `/proc/<pid>/status` for the original persistent server and the observable server.

### Observations

| Measurement | Persistent | Observable |
|---|---:|---:|
| VmRSS | 7848 KiB | 7952 KiB |
| VmHWM | 7848 KiB | 7952 KiB |
| Threads | 1 | 1 |

Observed RSS difference:

```text
104 KiB
```

This is approximately 1.3% relative to the original server's RSS.

### Interpretation

The instrumentation did not produce a large observed memory difference in this snapshot.

However, the processes had different uptimes and request histories.

The RSS difference cannot be attributed exclusively to instrumentation.

## 12. Experiment C — Monitoring Stack Resource Usage

### Procedure

Query systemd cgroup resource accounting using:

```bash
systemctl show prometheus \
  -p MemoryCurrent -p CPUUsageNSec -p TasksCurrent

systemctl show prometheus-node-exporter \
  -p MemoryCurrent -p CPUUsageNSec -p TasksCurrent

systemctl show grafana-server \
  -p MemoryCurrent -p CPUUsageNSec -p TasksCurrent
```

### Observations

Snapshot time:

```text
2026-10-13T10:53:50+09:00
```

| Component | MemoryCurrent | Approx. MiB | Tasks |
|---|---:|---:|---:|
| Prometheus | 68,952,064 bytes | 65.76 | 11 |
| Node Exporter | 10,108,928 bytes | 9.64 | 6 |
| Grafana | 480,407,552 bytes | 458.15 | 132 |
| Total | 559,468,544 bytes | 533.55 | — |

Cumulative CPU accounting:

| Component | CPUUsageNSec |
|---|---:|
| Prometheus | 22,335,243,000 |
| Node Exporter | 132,872,250,000 |
| Grafana | 52,595,722,000 |

The CPU figures are cumulative CPU time, not instantaneous utilization.

The monitoring stack consumed approximately 534 MiB of cgroup-accounted memory at the measured instant.

Cgroup memory is not directly equivalent to process RSS, and the measurements were not collected under an isolated steady-state workload.

## 13. Experiment D — HTTP Latency Overhead Comparison

### Hypothesis

Metrics instrumentation introduces some computational overhead, but the additional cost may be difficult to distinguish from database and scheduling variability.

### Procedure

Use:

```text
experiments/006-observability/compare_overhead.py
```

The script:

1. Warms up both HTTP servers with 20 requests each.
2. Runs six measurement rounds per server.
3. Sends 100 sequential read-only `/counter` requests per round.
4. Alternates server measurement order between rounds.
5. Uses the same Python HTTP client implementation.
6. Records mean, median, p95 and maximum latency.
7. Saves results to CSV.

Measured request counts:

```text
Persistent server: 600
Observable server: 600
Total:             1200
```

### Results

| Round | Persistent mean (ms) | Observable mean (ms) |
|---|---:|---:|
| 1 | 2.180 | 2.011 |
| 2 | 2.349 | 2.026 |
| 3 | 2.545 | 2.045 |
| 4 | 1.997 | 2.024 |
| 5 | 1.976 | 2.031 |
| 6 | 2.003 | 1.995 |

Summary:

| Measurement | Persistent | Observable |
|---|---:|---:|
| Mean of round means | 2.175 ms | 2.022 ms |
| Standard deviation of round means | 0.232 ms | 0.017 ms |
| Highest observed round p95 | 3.219 ms | 2.285 ms |
| Maximum request latency | 4.406 ms | 2.815 ms |

The observable server's measured mean was approximately 0.153 ms lower.

### Interpretation

This result does not demonstrate a performance improvement caused by observability.

In rounds 4–6, both servers exhibited approximately 2 ms mean latency.

The larger variation in the original server's first three rounds accounts for much of the overall difference.

No obvious instrumentation latency regression was resolved under this workload.

### Limitations

- Requests were sequential.
- Traffic was generated over loopback.
- Both implementations executed real database reads.
- The Python client introduced measurement overhead.
- CPU scheduling and background processes were not controlled.
- Prometheus regularly scraped the observable server.
- Only six rounds were collected.
- The experiment cannot isolate individual instrumentation instructions from total request latency.
- Results do not generalize to concurrent or production traffic.

## 14. Experiment E — Monitoring Failure Isolation

### Hypothesis

Prometheus can fail without preventing the HTTP application from serving database-backed requests.

### Procedure

1. Stop Prometheus using systemd.
2. Send requests directly to the observable server.
3. Restart Prometheus.
4. Verify that scraping resumes.

### Observations

While Prometheus was stopped:

```text
GET /health  -> HTTP 200, OK
GET /counter -> HTTP 200, 16
```

After restarting Prometheus:

```promql
up{job="resilient-pi"}
```

Returned:

```text
1
```

### Conclusion

The application continued serving requests during the monitoring outage.

Monitoring collection and application request handling have separate process lifecycles.

Missing telemetry does not necessarily imply application failure.

### Limitations

The experiment did not stop Grafana or Node Exporter.

Monitoring outage duration and the exact number of missed samples were not measured.

## 15. Security and Deployment Considerations

All three monitoring services were ultimately configured to listen on localhost:

```text
Grafana:       127.0.0.1:3000
Prometheus:    127.0.0.1:9090
Node Exporter: 127.0.0.1:9100
```

The observable C++ server also binds to localhost.

Remote Grafana access uses SSH port forwarding.

This reduces exposure of monitoring interfaces on the campus-facing network.

Authentication and authorization remain relevant for deployment beyond localhost access.

## 16. Final Findings

1. Application-level counters and histograms can expose useful service behavior.
2. Database readiness and HTTP liveness are independent properties.
3. HTTP errors can occur while host CPU usage remains low.
4. A cumulative error counter preserves evidence of failures after recovery.
5. Prometheus scraping does not imply full application readiness.
6. Histogram-based percentiles provide distribution information unavailable from averages alone.
7. Instrumentation showed a small observed RSS difference in the sampled processes.
8. The monitoring stack had a substantially larger memory footprint than the C++ instrumentation.
9. The sequential latency benchmark did not resolve a clear instrumentation penalty.
10. Prometheus failure did not stop the application from serving requests.

## 17. Outstanding Limitations and Future Work

- Correct the mismatch between request count and latency histogram population.
- Distinguish malformed requests and transport failures in telemetry.
- Improve the meaning of the active-connections gauge under single-threaded operation.
- Add status-class or bounded route labels if debugging requirements justify them.
- Investigate database readiness probe scheduling.
- Measure per-process CPU usage using interval-based sampling.
- Run a more rigorous overhead benchmark with controlled workload conditions.
- Measure metrics-rendering and scrape overhead directly.
- Add alerting based on application errors and readiness.
- Consider moving Prometheus and Grafana off-device for a memory-constrained edge deployment.
- Integrate observability with the V2 worker-pool implementation if it becomes the primary server.

## 18. Evidence Files

```text
server/observable_server.cpp
server/metrics.hpp

experiments/006-observability/metrics_smoke.cpp
experiments/006-observability/compare_overhead.py
experiments/006-observability/failure-test.md
experiments/006-observability/methodology.md
experiments/006-observability/metrics.md

observability/grafana/resilient-pi-dashboard.json

results/raw/day08/metrics-smoke.txt
results/raw/day08/metrics-before-failure.txt
results/raw/day08/metrics-during-db-failure.txt
results/raw/day08/metrics-after-recovery.txt
results/raw/day08/resource-snapshot.txt
results/raw/day08/overhead-latency.csv
results/raw/day08/overhead-summary.txt
results/raw/day08/monitoring-failure-test.md

results/figures/day08-observability-dashboard.png

journal/day08.md
docs/design-decisions.md
```

The evidence list represents intended repository artifacts. Verify actual file existence before staging the final commit.
