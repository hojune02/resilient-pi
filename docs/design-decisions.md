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
