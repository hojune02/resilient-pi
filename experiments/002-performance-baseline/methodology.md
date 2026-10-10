# Day 4 — HTTP Performance Benchmark Methodology

**Project:** Resilient Pi  
**Experiment:** 002 — Performance Baseline  
**Date:** October 8, 2026  
**Status:** Preliminary baseline collected; resource correlation and further validation pending

## 1. Objective

The purpose of this experiment is to establish a reproducible performance baseline for a single-threaded, blocking C++ HTTP server running on a Raspberry Pi 4 Model B.

Specifically, the experiment investigates:

1. How HTTP throughput changes as client concurrency increases.
2. How median and tail latency behave as concurrency increases.
3. At which concurrency level throughput stops scaling effectively.
4. Whether increasing concurrency improves throughput despite the server processing one connection at a time.
5. Whether CPU, memory, network behavior, or blocking I/O could explain observed limitations.
6. How stable the measurements are across repeated benchmark runs.

The experiment is intended to establish a baseline before modifying the server's concurrency architecture.

## 2. Test Environment

### Server hardware

| Property | Value |
|---|---|
| Device | Raspberry Pi 4 Model B Rev 1.4 |
| Architecture | ARM64 / AArch64 |
| Processor | Broadcom BCM2711, quad-core ARM Cortex-A72 |
| Reported usable memory | Approximately 7.6 GiB |
| Operating system | Ubuntu Server, ARM64 |
| Kernel | Linux 6.8.0-1065-raspi |
| Storage | 64 GB microSD |
| Network | Wired Ethernet |

The Pi was administered remotely through SSH.

### Client environment

| Property | Value |
|---|---|
| Device | Apple MacBook Pro |
| Benchmark tool | wrk |
| Benchmark execution | macOS terminal |
| Network connection | KAIST campus Wi-Fi |
| Server connection | KAIST campus routed network |

The MacBook generated requests to the Pi through the campus network. This means the measured request latency includes network transmission, routing, TCP behavior, and server-side processing.

### Server implementation

The benchmarked server was implemented in C++ using POSIX sockets.

Its request-processing sequence was:

```text
socket()
   ↓
setsockopt(SO_REUSEADDR)
   ↓
bind()
   ↓
listen()
   ↓
accept()
   ↓
recv()
   ↓
send()
   ↓
close(client)
   ↓
accept next client
```

The application uses a single thread and processes accepted connections sequentially.

The server listens on TCP port 8080 and returns a minimal HTTP response:

```http
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 3
Connection: close

OK
```

The server closes the TCP connection after each response.

**Important implementation characteristics:**

- Blocking `accept()` and `recv()`
- No application-level concurrent request handling
- Configured receive timeout in the baseline implementation
	- The benchmarked V1 implementation configured `SO_RCVTIMEO`
to one second on each accepted client socket.

This limits individual blocking receive operations but
does not provide concurrent client handling.

All Day 4 measurements were collected using this V1
implementation, according to the recorded deployment
procedure.

The running executable's SHA-256 was not captured
at the time of every benchmark, which limits exact
binary-level reproducibility.
- One `recv()` call per accepted connection
- One `send()` call per response
- Request data logged to standard output
- No HTTP keep-alive support
- No database or external dependency

These characteristics are intentionally preserved as the initial baseline. They introduce known limitations and provide opportunities for later controlled improvements.

## 3. Benchmark Tool

The load generator was `wrk`, executed on the MacBook.

A representative benchmark command was:

```bash
wrk -t2 -c25 -d15s --latency --timeout 2s \
  http://SERVER_IP:8080/health
```

Parameters:

| Parameter | Meaning |
|---|---|
| `-t2` | Two load-generator threads |
| `-c25` | Twenty-five concurrent connections |
| `-d15s` | Fifteen-second measurement duration |
| `--latency` | Report latency distribution |
| `--timeout 2s` | Two-second timeout |

The server address is intentionally represented as `SERVER_IP` in this document. The actual address is stored in local benchmark configuration rather than being required for reproducing the experiment.

Because the server closes each connection after responding, results include repeated TCP connection establishment and teardown overhead.

The benchmark therefore measures the performance of the complete HTTP/TCP interaction rather than isolated C++ request-handler execution time.

## 4. Experimental Variables

### Independent variable

The primary independent variable was the number of concurrent client connections.

The tested values were:

| Configuration | Connections | wrk threads |
|---|---:|---:|
| B01 | 2 | 2 |
| B02 | 10 | 2 |
| B03 | 25 | 2 |
| B04 | 50 | 2 |
| B05 | 100 | 2 |

One-connection tests were performed during preliminary exploration but were excluded from the standardized two-thread concurrency sweep.

The original project plan considered testing 250 and 500 connections. These were not necessary to identify the preliminary throughput plateau and were not included in the standardized benchmark.

### Controlled variables

The following settings were held constant during the standardized sweep:

- Same Raspberry Pi hardware
- Same server implementation
- Same HTTP endpoint and response payload
- Same MacBook as load generator
- Same campus network path
- Two wrk threads
- Fifteen-second measurement duration
- Five-second warm-up run
- Ten-second rest period between repeated trials
- Two-second connection timeout

Three measurement trials were executed for each concurrency level.

### Dependent variables

The primary observed metrics were:

- Completed requests per second
- p50 request latency
- p99 request latency
- Reported socket errors
- Reported request timeouts

Additional exploratory monitoring collected:

- Process CPU utilization
- User versus system CPU time
- Resident set size (RSS)
- Minor and major page faults

These resource measurements were not fully synchronized with every benchmark trial.

## 5. Experimental Procedure

For each concurrency level:

1. Confirm the C++ server is running and responding to HTTP requests.
2. Execute a five-second warm-up workload.
3. Execute a fifteen-second measured workload.
4. Preserve the complete wrk terminal output.
5. Extract throughput, p50, p99, and reported error counts.
6. Append the results to a CSV file.
7. Wait ten seconds before beginning the next repetition.
8. Repeat the process three times.

The benchmark was executed remotely to avoid running the primary load generator on the Raspberry Pi itself.

The resulting CSV uses the following schema:

```csv
timestamp,threads,connections,trial,requests_per_sec,p50_ms,p99_ms,errors,timeouts
```

The initial benchmarking script used filenames of the form:

```text
raw/c25_trial1.txt
```

This naming convention can overwrite earlier results when a configuration is repeated.

Subsequent experiments should use a unique run identifier or timestamp and retain all original measurement outputs.

## 6. Initial Results

The following table summarizes the first standardized set of three trials per concurrency level.

| Connections | Mean throughput (req/s) | Mean trial p50 (ms) | Mean trial p99 (ms) |
|---|---:|---:|---:|
| 2 | 260.19 | 3.51 | 8.19 |
| 10 | 485.79 | 9.85 | 20.28 |
| 25 | 499.30 | 23.21 | 223.20 |
| 50 | 487.17 | 50.98 | 77.33 |
| 100 | 456.37 | 108.21 | 165.71 |

The reported latency values are **means of per-trial percentiles**, not percentiles recomputed from pooled individual-request observations.

At 25 connections, one trial recorded p99 latency of 589.66 ms. This strongly influenced the mean p99 value.

### Observed throughput behavior

Throughput increased substantially between 2 and 10 concurrent connections.

Between 10 and 50 connections, measured throughput remained around 485–500 requests per second.

At 100 connections, mean throughput was lower, while latency continued increasing.

This suggests that the tested end-to-end system approaches a throughput plateau at approximately 10–25 concurrent connections.

The experiment does not conclusively establish whether this limit comes from the C++ implementation, the client, TCP connection overhead, campus networking, or another resource.

### Observed latency behavior

Request latency increased as concurrency increased.

This suggests that adding more connections increased waiting time without producing proportional throughput gains.

A likely explanation is queueing in an end-to-end system whose service rate is limited.

However, identifying the actual limiting component requires additional measurements.

## 7. Repeatability and Anomalies

### Initial 10-connection repeatability experiment

Three repeated 10-connection trials produced:

| Trial | Throughput (req/s) |
|---|---:|
| 1 | 501.81 |
| 2 | 478.98 |
| 3 | 476.58 |

The mean throughput was approximately 485.79 requests per second.

This variation demonstrates why a single benchmark run should not be interpreted as an exact capacity measurement.

### 25-connection tail-latency anomaly

The initial 25-connection trial recorded:

```text
p50:       23.10 ms
p90:       29.40 ms
p99:      589.66 ms
maximum:  746.40 ms
```

The unusually large difference between p90 and p99 indicated that a minority of requests experienced severe delays.

The original wrk output confirmed the reported p99 value was not a CSV parsing error.

A subsequent three-trial run at 25 connections produced:

| Trial | Throughput (req/s) | p99 (ms) |
|---|---:|---:|
| 1 | 504.09 | 45.21 |
| 2 | 410.82 | 102.20 |
| 3 | 365.42 | 81.19 |

The original 589.66 ms p99 spike was not reproduced in this batch, although considerable variability remained.

The later run also showed decreasing throughput across its three trials.

Potential causes include:

- Temporary campus-network congestion
- TCP retransmissions or connection establishment delays
- Blocking application-level I/O
- Operating-system scheduling variation
- Benchmark-client behavior
- Request logging overhead

No definitive root cause was established.

The anomaly must therefore be preserved as an observation, not described as a resolved defect.

## 8. Localhost Versus Remote Benchmark

An exploratory localhost benchmark executed directly on the Raspberry Pi used:

```bash
wrk -t1 -c10 -d10s --latency --timeout 2s \
  http://127.0.0.1:8080/health
```

It produced:

```text
Throughput: 3138.23 requests/sec
Mean latency: 1.43 ms
p99 latency: 4.07 ms
```

The remote 10-connection benchmark produced substantially lower throughput.

This difference suggests that network-path characteristics and connection/client behavior contribute materially to the observed remote performance.

However, the two experiments used different wrk thread counts and test durations.

Furthermore, the localhost benchmark ran both the load generator and server on the same Raspberry Pi, introducing CPU resource competition.

Therefore, the localhost comparison is exploratory and cannot independently establish that the campus network is the sole bottleneck.

A future experiment should use matched benchmarking configurations and, ideally, an independent load generator connected over a controlled local network.

## 9. Resource Utilization

Exploratory `pidstat` monitoring indicated relatively low CPU consumption by the C++ server, approximately 3–9% in sampled intervals.

The reported RSS was approximately 2,816 KB and remained stable across the observed windows.

Most measured application CPU consumption appeared as system CPU time rather than user CPU time.

This suggests the server was not saturating a CPU core under the observed remote workloads.

However, measurements were not perfectly synchronized with every benchmark trial, and the total CPU utilization of the complete system was not established for every configuration.

**Current conclusion:** CPU exhaustion is not supported as the primary explanation by the available process measurements, but the actual bottleneck remains unconfirmed.

## 10. Benchmark Limitations

### Limited repetitions

Three repetitions per configuration are useful for identifying obvious variability but do not establish precise confidence intervals.

Additional independent runs would be required for statistically stronger conclusions.

### Network variability

The MacBook communicates with the server across the KAIST campus network.

Unlike a dedicated local test network, this introduces traffic, routing, and latency variability outside the experimenter's control.

### HTTP connection behavior

The server closes the connection after each response.

Therefore, the experiment measures a connection-heavy HTTP workload rather than persistent HTTP connection performance.

### Simplified HTTP implementation

The C++ server is not a complete HTTP/1.1 implementation.

It does not yet correctly handle all fragmented requests, partial sends, connection lifecycle conditions, or request validation.

The endpoint responds with HTTP 200 when positive request bytes are received, regardless of the requested path.

### Response validation

The initial benchmark parser records socket errors when wrk reports them, but treats the absence of a socket-error line as zero errors.

This does not independently verify that every completed HTTP response is semantically correct.

Future tests should validate HTTP response status, response body, and separate transport failures from application-level failures.

### Missing p95 measurements

The initial wrk output reports p50, p75, p90 and p99, but does not provide p95 in the collected format.

No p95 values are inferred or fabricated.

A future benchmark tool configuration should explicitly collect p95.

### Measurement overhead

Application logging, benchmark instrumentation, and system monitoring can affect performance.

Their overhead has not yet been isolated.

### No synchronized measurements for every trial

CPU, RAM and TCP statistics were collected during exploratory monitoring but were not systematically aligned with every benchmark run.

A future experiment should record timestamps and resource metrics over precisely matching measurement windows.

### Benchmark scope

The tested endpoint performs almost no application computation and does not access a database.

The results therefore do not represent production workloads involving business logic, persistent storage or external services.

## 11. Preliminary Conclusions

The experiment supports the following observations:

1. Increasing concurrency initially improves throughput even though the server processes connections sequentially.
2. Throughput begins to plateau around 10–25 concurrent connections in the remote test environment.
3. Increasing concurrency beyond that point substantially raises request latency without comparable throughput improvement.
4. The system exhibits occasional extreme tail-latency observations.
5. Remote performance is substantially lower than localhost performance, but the precise contribution of network and client behavior has not been isolated.
6. Available process-level monitoring does not support CPU saturation as the primary limiting factor.
7. Repeated measurements are necessary to distinguish typical behavior from run-to-run variation.

The experiment does **not** establish a definitive maximum throughput for the Raspberry Pi hardware.

It establishes a preliminary performance baseline for this specific C++ implementation, HTTP workload, load generator and network environment.

## 12. Next Experiments

The following investigations would help identify the limiting component:

**Experiment A — Controlled local versus remote testing**

Use matched wrk parameters and compare throughput, latency and socket errors.

**Experiment B — Logging overhead**

Compare identical workloads with verbose request logging enabled and disabled.

**Experiment C — Slow-client isolation**

Investigate how a blocking `recv()` affects unrelated clients when one connection remains idle.

**Experiment D — Receive timeout**

Measure whether a bounded receive timeout limits slow-client-induced unavailability.

**Experiment E — Concurrent server architecture**

Compare the baseline implementation against a bounded worker pool, preserving the same load-generation methodology.

These should be treated as independent experiments rather than changing several variables simultaneously.

## 13. Evidence and Reproduction

Repository locations:

```text
experiments/002-performance-baseline/
    run.sh
    methodology.md

results/raw/day04/
    baseline.csv
    wrk/
    system-info.txt
    systemd-unit.txt

results/figures/
    throughput-vs-concurrency.png
    latency-vs-concurrency.png

journal/
    day04.md
```

The canonical repository should retain the benchmark scripts, configuration, raw observations, processed results and explanations of limitations.

Benchmark results should be associated with the exact source-code commit and environment versions whenever possible.

## 14. Engineering Reflection

My initial prediction was that increasing client concurrency would reduce throughput because the server handles one connection at a time.

The measurements contradicted that prediction: throughput initially increased substantially with concurrency.

This revealed that sequential request processing does not necessarily imply decreasing throughput when concurrency rises. Multiple clients can keep work available in kernel-managed connection queues, reducing application idle time.

Further experimentation demonstrated that throughput and latency do not scale in the same way. After throughput approached a plateau, increasing concurrency primarily increased waiting time.

The key lesson from this experiment is:

**Performance claims require controlled measurements, repeatability, and a clear distinction between observed symptoms and demonstrated root causes.**
