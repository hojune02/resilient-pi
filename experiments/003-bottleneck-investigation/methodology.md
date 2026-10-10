# Experiment 003 — Bottleneck Investigation

**Project:** Resilient Pi  
**Measurement date:** 2026-10-10 (KST; Day 5 work performed ahead of the roadmap's 11 Oct date)  
**Status:** Completed exploratory network-path comparison; application throughput root cause remains open.  
**Implementation:** V1 single-threaded blocking C++ TCP/HTTP server, with `SO_REUSEADDR` and `SO_RCVTIMEO = 1 s` on accepted client sockets. No server-code changes were made during the Day 5 comparisons, as reported.

## 1. Research question

At a fixed 25-connection HTTP workload, does accessing the same Raspberry Pi server through Tailscale materially change throughput and latency compared with direct access over the home LAN? What do system resource measurements suggest about the first bottleneck?

This is an **end-to-end network-path experiment**, not a pure application-server microbenchmark. The two paths share the server, MacBook load generator, and physical Wi-Fi environment, but differ in overlay routing and packet processing.

## 2. Prior evidence and hypotheses

Day 4, conducted on the KAIST network, showed throughput near 400–500 requests/s in several remote concurrency tests, rising latency with greater concurrency, and relatively low application-process CPU utilization. Those results cannot be treated as a matched control for the home-network experiments.

Hypotheses considered:

| ID | Hypothesis | Expected evidence | Status after Day 5 |
|---|---|---|---|
| H-NET | Tailscale path imposes meaningful end-to-end overhead relative to direct home LAN | Lower throughput/higher latency via Tailscale with unchanged server and load settings | Supported for the measured workload; source of overhead not isolated |
| H-CPU | C++ process saturates a CPU core | Server process near 100% of a single core | Not supported by available process samples |
| H-MEM | Memory pressure constrains throughput | Increasing RSS, swapping, major faults | Not supported by available samples |
| H-LOG | Request logging is a significant contributor | Logging services consume CPU; disabling logging under controlled conditions improves performance | Plausible; causal test not performed |
| H-SEQ | Sequential blocking I/O prevents unrelated clients from being handled while one client is stalled | Idle accepted client delays other requests despite spare CPU | Previously demonstrated separately; 1 s receive timeout bounds one `recv()` wait, not concurrency |
| H-BACKLOG | Accept queue saturates | Sustained listening `Recv-Q` growth approaching `Send-Q` | Not demonstrated in sampled socket states |

## 3. Test system and controls

- Server: Raspberry Pi 4 Model B, four ARM CPU cores, approximately 8 GB RAM; Ubuntu Server ARM64; port 8080.
- Service: `resilient-pi.service` managed by systemd; executable `server/server` is deliberately Git-ignored.
- Client: MacBook running `wrk`, with the same load generator and command options for both routes.
- Direct LAN target at measurement time: `192.168.0.57:8080`.
- Tailscale target at measurement time: `100.92.165.63:8080`.
- Tailscale status/ping: **direct peer path** reported as `192.168.0.1:21388`; not a DERP-relayed path in the observed checks. A direct Tailscale path still uses encryption and overlay processing.
- Fixed benchmark parameters: **2 wrk threads, 25 open connections, 15 s measurement, 2 s socket timeout**, HTTP `/health`.
- Automated comparison: **5 s warm-up**, **10 s cooldown**, **three measured repetitions per path**.
- Trial ordering: **LAN, TAILSCALE, TAILSCALE, LAN, LAN, TAILSCALE** (ABBAAB).
- The first ad hoc one-trial LAN/Tailscale comparison was exploratory and **is not included in the six-trial summary**.

Executed benchmark shape:

```bash
wrk -t2 -c25 -d15s --latency --timeout 2s http://TARGET:8080/health
```

Automation and analysis files: `results/raw/day05/runs/compare-networks.sh`, `results/raw/day05/runs/summarize.py`. Original run files: `results/raw/day05/runs/20261010_101930/`.

### Reproducibility provenance

Retain the recorded `results/raw/day05/source-commit.txt`, `binary-sha256.txt`, `compiler.txt`, `systemd-unit.txt`, and `service-state.txt`. They describe the captured configuration; **a binary hash collected after earlier runs does not retroactively prove the executable bytes used for each earlier benchmark**. Verify commit, build options, binary hash and runtime PID before new comparisons.

## 4. Measurements

The summary below was generated from the six-trial `results.csv` as reported by `summarize.py`:

| Metric (across 3 trials per path) | LAN | Tailscale |
|---|---:|---:|
| Throughput mean (requests/s) | **730.00** | **450.24** |
| Throughput sample SD (requests/s) | 5.96 | 11.95 |
| Throughput range (requests/s) | 724.24–736.14 | 437.38–461.01 |
| Mean of per-run p50 (ms) | 15.45 | 25.61 |
| Sample SD of per-run p50 (ms) | 0.34 | 0.56 |
| Mean of per-run p99 (ms) | 31.89 | 47.78 |
| Sample SD of per-run p99 (ms) | 0.96 | 1.21 |

Derived comparisons:

- LAN throughput exceeded Tailscale throughput by **279.76 requests/s**, or **62.1% relative to Tailscale throughput**.
- LAN mean p50 was **39.7% lower** than Tailscale mean p50.
- LAN mean run-level p99 was **33.3% lower** than Tailscale mean run-level p99.
- The three LAN throughput measurements did not overlap the three Tailscale throughput measurements.

**Statistical caveat:** The latency values are averages of **three run-level quantiles**, not pooled request-level quantiles. Three runs per route give a clear exploratory difference but are insufficient for general claims across times, devices, or network conditions. No formal significance test was planned.

### Exploratory single-trial results (excluded above)

| Path | Requests/s | p50 (ms) | p99 (ms) |
|---|---:|---:|---:|
| LAN | 546.90 | 21.11 | 56.93 |
| Tailscale | 524.58 | 22.25 | 43.50 |

An earlier Tailscale-only run achieved 585.49 requests/s, p50 19.59 ms and p99 40.61 ms. The spread across sessions reinforces that environmental and measurement conditions matter; the six-trial batch is not representative of every session.

## 5. Host observations

In a Tailscale workload, sampled process CPU usage included:

- `tailscaled`: approximately **115–119%** of a single CPU core in several one-second samples.
- `server`: approximately **10–13%** of one core.
- `systemd-journald`: approximately **26–28%** of one core.
- `rsyslogd`: approximately **24–27%** of one core.
- Broadcom Wi-Fi-related kernel worker: approximately **25–26%** of one core.

These are **selected samples, not matched-run averages**. Linux `pidstat` can report more than 100% for a multithreaded process: 115% corresponds to 1.15 logical cores of CPU time (roughly 28.75% of a four-core machine's aggregate capacity). Do not sum selected per-process values and call them total CPU usage. Kernel work may be attributed differently depending on how it executes.

Earlier synchronized `vmstat` during the Tailscale load showed approximately **44–50% aggregate idle CPU**, substantial system CPU time and tens of thousands of context switches per second, but no swapping. Server RSS was about **2,816 KiB**. Sampled listening socket state included `LISTEN 0 4096` on `0.0.0.0:8080`, indicating no pending accepted-connection backlog at those sample instants, **not** absence of short-lived queue peaks.

`ss -tan ... | wc -l` returning `1` after the test could count only the header; repeat with `ss -H` and sample **during** load before interpreting TIME_WAIT.

## 6. Interpretation and limitations

**Supported:** Route selection strongly affected measured end-to-end throughput and latency in the six-trial comparison; direct LAN was consistently faster. The HTTP server's own CPU and memory usage did not show obvious saturation in the available monitored Tailscale run.

**Not established:** Exactly how much of the gap came from Tailscale userspace processing, cryptography, TCP behavior, Wi-Fi, logging, routing, scheduling, or background workload. `tailscaled` CPU use correlates with traffic and is a promising lead, but high CPU alone is not proof that it is the throughput-limiting component. Additionally, the six-route comparison did not include synchronized per-process CPU measurements in **both** paths.

**Threats to validity:** Wi-Fi interference, possible MacBook/Pi background tasks, CPU frequency/thermal variation, session-to-session drift, wrk's connection behavior, unvalidated HTTP body correctness, no per-request tracing, small sample size, and imperfect provenance for older runs. A `wrk` run with no reported socket/status errors is not a full HTTP correctness test.

The single-threaded blocking design also remains a demonstrated **availability/slow-client limitation** even with a 1-second receive timeout. That distinct failure should not be conflated with the measured 25-connection steady-state throughput gap.

## 7. Follow-up experiment (separate from baseline)

1. Keep V1 binary and benchmark parameters fixed; capture synchronized per-process `pidstat` for LAN **and** Tailscale at the same concurrency. Measure connection counts, queue occupancy and aggregate CPU simultaneously.
2. Repeat the route comparison at additional concurrency values with multiple trials to check for a throughput knee and rising errors/tail latency; don't claim full saturation from only the c25 route comparison.
3. After committing this baseline, create a **versioned** logging-on/off comparison using direct LAN; do not silently change the original code or results.
4. Day 6: compare the current sequential server with one bounded-concurrency implementation on the **same direct-LAN path**, explicitly testing slow-client isolation, overload behavior, throughput, latency and resource cost.

## 8. Raw evidence index

- `results/raw/day05/runs/20261010_101930/results.csv` and six numbered `*.txt` files — paired route-comparison evidence.
- `results/raw/day05/runs/compare-networks.sh` — fixed benchmark procedure.
- `results/raw/day05/runs/summarize.py` — calculation of run-level means and sample SDs.
- `results/raw/day05/home-network/{pidstat-c25.log,vmstat-c25.log,socket-c25.log,environment.txt}` — monitored home-network/Tailscale test.
- `results/raw/day05/{pidstat-c25.log,vmstat-c25.log,socket-c25.log}` — original monitored copies; verify duplication before deduplicating.
- `results/raw/day05/{binary-sha256.txt,compiler.txt,source-commit.txt,service-state.txt,systemd-unit.txt}` — recorded provenance.
- `docs/design-decisions.md` — design implications and decisions, maintained separately from measurement notes.
