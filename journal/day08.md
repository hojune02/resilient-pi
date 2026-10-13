
# Day 8 — Observability: Metrics, Prometheus and Grafana

**Date:** 2026-10-13  
**Roadmap date:** 2026-10-14  
**Project:** Resilient Pi

## 1. Today's Goal

Make the PostgreSQL-backed C++ server observable.

By the end of the session, the system should expose HTTP and database metrics, collect host-level resource measurements, display time-series data, and provide evidence about failures and monitoring overhead.

The goal is to understand the signals and their limitations rather than merely install monitoring software.

## 2. Starting Point

Day 7 produced a PostgreSQL-backed HTTP server supporting:

```text
GET /health
GET /counter
POST /counter/increment
```

The application maintained a persistent PostgreSQL connection and could recover connectivity after a database restart.

However, diagnosing failures required direct HTTP tests and terminal inspection.

There was no time-series history of application errors or database readiness.

## 3. Instrumentation Design

I copied the working Day 7 implementation into:

```text
server/observable_server.cpp
```

The new server listens on:

```text
127.0.0.1:8082
```

I implemented fixed-bucket histograms and application counters in:

```text
server/metrics.hpp
```

I first tested the metrics independently using known synthetic observations.

The histogram correctly recorded three durations totaling 0.165 seconds.

After integration, real HTTP requests changed the exported counters and histograms as expected.

## 4. Important Finding: Measurement Semantics

The `/metrics` request itself is an HTTP connection.

Because the server increments its active-connection gauge after `accept()`, Prometheus typically observes:

```text
resilient_http_active_connections 1
```

This does not mean that another client request is being processed concurrently.

The server is single-threaded.

This demonstrated that a metric can be technically accurate yet have limited diagnostic usefulness.

I also identified a difference between HTTP response attempts and accepted connections.

The request counter counts response attempts, while the latency histogram may include accepted connections that never receive a response.

A future revision should make the measurement populations consistent.

## 5. Database Readiness

I added:

```text
GET /ready
```

Unlike `/health`, readiness requires a successful PostgreSQL round trip.

When PostgreSQL was healthy:

```text
GET /health  -> HTTP 200
GET /ready   -> HTTP 200
GET /counter -> HTTP 200, 16
```

The first measured database operation took approximately 2.226 ms.

The database operation duration metric includes reconnection behavior and should not be described as pure SQL execution time.

## 6. PostgreSQL Failure Injection

I stopped PostgreSQL while leaving the C++ server running.

During the outage:

```text
GET /health  -> HTTP 200
GET /ready   -> HTTP 503
GET /counter -> HTTP 503
```

Observed:

```text
resilient_http_requests_total 8
resilient_http_errors_total 2
resilient_db_connection_up 0
resilient_db_operation_duration_seconds_count 2
```

After restarting PostgreSQL:

```text
GET /ready   -> HTTP 200
GET /counter -> HTTP 200, 16
```

Observed:

```text
resilient_http_requests_total 11
resilient_http_errors_total 2
resilient_db_connection_up 1
resilient_db_operation_duration_seconds_count 3
```

### Learning

The process can remain alive while a required dependency is unavailable.

A readiness signal and an error counter provide more useful evidence of the outage than CPU utilization alone.

Counters preserve historical failures after recovery.

## 7. Prometheus Installation

I installed Prometheus and configured it to scrape the observable server every five seconds.

I verified:

```promql
up{job="resilient-pi"}
```

returned:

```text
1
```

I also configured Prometheus to collect Node Exporter metrics.

Initially, Prometheus listened on every network interface.

I changed its systemd configuration so it listened on:

```text
127.0.0.1:9090
```

This reduced unnecessary exposure of the monitoring endpoint.

## 8. Node Exporter Installation

Node Exporter provided host CPU, memory and load-average measurements.

I verified the following metrics:

```text
node_cpu_seconds_total
node_memory_MemAvailable_bytes
node_memory_MemTotal_bytes
node_load1
```

The Pi reported approximately 8 GB physical RAM and four CPU cores.

Node Exporter initially listened on all interfaces. I changed its configuration and verified that it listened on:

```text
127.0.0.1:9100
```

### Learning

Host metrics are not process metrics.

High host CPU usage does not identify which application caused the load.

Low CPU usage does not establish that requests are succeeding.

## 9. Grafana Installation and Dashboard

Grafana was not available through the initial Ubuntu package indexes.

I added Grafana's package repository, installed the service, and connected Grafana to Prometheus.

Grafana initially listened on all interfaces.

I corrected the configuration so it listened on:

```text
127.0.0.1:3000
```

The dashboard contains:

1. HTTP throughput
2. HTTP error rate
3. HTTP p95 latency
4. Database availability
5. Database operation p95 latency
6. Active HTTP connections
7. Host CPU utilization
8. Host available memory

I used a version-controlled JSON dashboard definition.

### Import problem

The initial dashboard JSON referenced:

```text
${DS_PROMETHEUS}
```

Grafana could not resolve this placeholder.

I replaced it with the actual Prometheus data source UID and reimported the dashboard.

The panels then displayed real metrics.

### Learning

A reproducible dashboard requires both query definitions and correct data source references.

A working monitoring stack can still fail at the visualization configuration layer.

## 10. Process Memory Measurements

I inspected the resident memory of the persistent and observable C++ servers.

Observed:

| Server | RSS |
|---|---:|
| Persistent | 7848 KiB |
| Observable | 7952 KiB |

The observable server used 104 KiB more RSS in that snapshot.

The two processes had different runtimes and workloads, so the difference cannot be treated as an isolated measurement of instrumentation overhead.

## 11. Monitoring Stack Cost

At 10:53:50 KST, I recorded:

| Component | Approximate cgroup memory |
|---|---:|
| Prometheus | 65.76 MiB |
| Node Exporter | 9.64 MiB |
| Grafana | 458.15 MiB |
| Total | 533.55 MiB |

Grafana consumed substantially more memory than the C++ application.

This suggests that observability architecture should account for the resources consumed by the monitoring infrastructure, not only instrumentation inside the application.

For a resource-constrained edge deployment, running Prometheus and Grafana elsewhere may be worth evaluating.

## 12. Observability Overhead Benchmark

I compared:

```text
Persistent server: 127.0.0.1:8081
Observable server: 127.0.0.1:8082
```

The benchmark executed 600 measured read-only requests per server.

It used six rounds, 100 requests per round, and alternating test order.

Results:

| Round | Persistent mean | Observable mean |
|---|---:|---:|
| 1 | 2.180 ms | 2.011 ms |
| 2 | 2.349 ms | 2.026 ms |
| 3 | 2.545 ms | 2.045 ms |
| 4 | 1.997 ms | 2.024 ms |
| 5 | 1.976 ms | 2.031 ms |
| 6 | 2.003 ms | 1.995 ms |

Summary:

```text
Persistent mean: 2.175 ms
Observable mean: 2.022 ms
```

### Interpretation

The observable server had a lower mean in this experiment, but this does not demonstrate that observability improved performance.

In later rounds, both servers were close to 2 ms per request.

Background scheduling, database operations and client overhead can contribute to the variation.

The experiment did not identify a clear instrumentation-related latency regression.

## 13. Monitoring Failure Isolation

I stopped Prometheus and tested the C++ server directly.

Observed while Prometheus was stopped:

```text
GET /health  -> HTTP 200
GET /counter -> HTTP 200, 16
```

After restarting Prometheus:

```promql
up{job="resilient-pi"}
```

returned:

```text
1
```

### Learning

Monitoring infrastructure should not be required for normal application operation.

The application continued serving requests while historical metric collection was interrupted.

Missing telemetry does not necessarily mean the application is unavailable.

## 14. Engineering Questions

### Q1. What question does each metric answer?

- HTTP request counter: How many requests has the application attempted to answer?
- HTTP error counter: How frequently are error responses occurring?
- HTTP send failures: Are response transmissions failing?
- HTTP latency histogram: How long are accepted connections taking to process?
- Active connections: How many accepted sockets are currently being handled?
- Process uptime: How long has the current server instance existed?
- Database state: Was the most recently checked database operation successful?
- Database operation histogram: How long are database-backed operations taking?
- Host CPU: How busy are the machine's CPU cores?
- Available memory: How much memory headroom remains?
- Prometheus `up`: Was the latest scrape successful?

A useful metric should be linked to a question or operational decision.

### Q2. Which metrics are symptoms and which are causes?

HTTP error rate and request latency are service-level symptoms.

Database availability and database-operation latency provide evidence about a dependency.

CPU and memory measurements describe resource conditions that may contribute to failures.

No single metric proves root cause by itself.

For example, high CPU may correlate with latency but does not establish causation.

### Q3. Which metric would tell you users are suffering before CPU hits 100%?

HTTP error rate or HTTP tail latency.

During the PostgreSQL outage, the application produced HTTP 503 responses while remaining alive.

User-visible failures do not require CPU saturation.

### Q4. What is high-cardinality telemetry, and why is it dangerous?

High-cardinality telemetry creates many distinct metric label combinations.

Examples include labels containing user IDs, raw URLs, request IDs or session IDs.

Each unique label combination can become a separate time series, increasing memory usage, storage requirements and query costs.

I therefore kept the current application's metrics at fixed cardinality.

### Q5. Why is a latency histogram better than only logging average latency?

An average hides the distribution of request durations.

For example, many fast requests and a few very slow requests can produce a moderate average even when some clients experience severe delays.

A histogram preserves counts across latency ranges and supports approximate p95/p99 estimation.

Its accuracy depends on bucket selection and sample volume.

### Q6. What should you log when a request fails?

Useful fields include:

- Timestamp
- Error category
- HTTP method
- Bounded route identifier
- Status code
- Request duration
- Database operation state
- Safe correlation identifier when available

The log should help reproduce or diagnose the failure without unnecessarily exposing request data.

### Q7. What should you deliberately not log?

Secrets and sensitive information such as:

- Passwords
- Authentication tokens
- Cookies
- Authorization headers
- API keys
- Database credentials
- Unnecessary personal information
- Raw request bodies unless explicitly justified and protected

Minimizing logged information reduces security and privacy risks.

### Q8. How much overhead does the observability stack introduce on a 4 GB system?

I measured approximately 533.55 MiB of combined cgroup memory for Prometheus, Node Exporter and Grafana on my approximately 8 GB Pi.

Relative to a nominal 4 GiB memory budget, that snapshot would represent approximately 13% of physical memory.

However, I did not run this experiment on a 4 GB system.

The figure is an extrapolation from one measured snapshot, not a verified 4 GB deployment result.

The observable C++ process had 104 KiB higher RSS than the original process in a preliminary comparison.

The latency benchmark did not resolve a clear instrumentation penalty.

### Q9. Can the monitoring system fail independently of the application?

Yes.

I stopped Prometheus while the HTTP server remained running.

The application continued returning HTTP 200 for both `/health` and `/counter`.

After restarting Prometheus, its scrape target returned `up = 1`.

### Q10. If metrics disappear, does that mean the application is down?

No.

Missing metrics may result from:

- Prometheus failure
- Exporter failure
- Network problems
- Scrape configuration errors
- Delayed scrapes
- Application failure

The monitoring pipeline itself has failure modes.

An independent application-level probe is needed to distinguish these cases.

## 15. Design Decisions

I chose lightweight in-process instrumentation with external Prometheus collection and Grafana visualization.

Reasons:

- The application is small enough to instrument directly.
- Fixed-cardinality metrics are relatively simple to reason about.
- Prometheus provides historical data without modifying database business logic.
- Grafana provides reusable visualization.
- Monitoring components can fail independently from the application.
- The architecture preserves an opportunity to move monitoring off-device later.

I did not add OpenTelemetry tracing because the current deployment consists of a small number of local processes and the existing metrics answer the primary operational questions.

Tracing may become useful if the system grows into multiple networked services.

## 16. Personal Reflection

**What I initially misunderstood about observability:**  
TODO — complete using my own experience.

**Which metric was least useful and why:**  
TODO — consider `resilient_http_active_connections`.

**What was the most important failure observed:**  
TODO — describe either the database-readiness failure or monitoring isolation.

**What would I change for a production deployment:**  
TODO — discuss error classification, readiness and monitoring resource constraints.

**Which measurements are still insufficient:**  
TODO — discuss benchmark repeatability, process CPU, tail latency and monitoring overhead.

## 17. Day 8 Outcome

The application now exposes real request and database metrics.

Prometheus collects the metrics, Node Exporter provides host telemetry, and Grafana displays the resulting time series.

A PostgreSQL failure was observable through readiness and error metrics.

The monitoring stack was shown to fail independently from the application.

The implementation demonstrated useful observability concepts while revealing limitations in metric semantics, measurement overhead and resource consumption.

## 18. Next Steps

- Preserve dashboard and experiment evidence in Git.
- Fix inconsistent request/histogram accounting.
- Improve readiness-state freshness.
- Add status-class metrics and alerts.
- Evaluate process-specific CPU and memory exporters.
- Investigate off-device monitoring for constrained deployments.
- Revisit the pending Day 6 direct-Ethernet V1/V2 benchmark when appropriate hardware is available.
