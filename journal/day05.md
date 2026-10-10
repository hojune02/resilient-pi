# Day 05 — Identify the First Real Bottleneck

**Work performed:** 2026-10-10 (KST)  
**Roadmap date:** 2026-10-11  
**State:** Evidence gathered; root-cause isolation and design decision still open.

> **Journal integrity:** Measurements and actions below are reconstructed from the terminal outputs shared during Day 5. The *Reflection* prompts are deliberately left for me to answer in my own words. I must not present generated interpretations as discoveries I personally made without checking them.

## Objective

Push V1 far enough to observe throughput/latency limits, identify the first failure mode, inspect CPU, run queue, context switching, TCP queues and network activity, and form a falsifiable bottleneck hypothesis **before** changing the implementation.

## Starting configuration

- Raspberry Pi 4 Model B, four CPU cores, Ubuntu Server ARM64.
- C++ HTTP server: single-threaded, blocking socket calls, `SO_REUSEADDR`, and accepted-client `SO_RCVTIMEO = 1 s`.
- systemd manages `resilient-pi.service`.
- Pi moved from the KAIST network to home Wi-Fi. MacBook accessed it over direct home LAN or direct-peer Tailscale.
- Day 4 campus-network measurements are **not** a controlled baseline for this home-network comparison.
- Day 5 source/binary/compiler/service metadata saved in `results/raw/day05/` (see methodology for provenance limitation).

## Experiment timeline (facts from recorded outputs)

1. Collected `pidstat`, `vmstat` and `ss` during a home-network benchmark addressed to the Pi's Tailscale IP.
2. The initial Tailscale c25 `wrk` run yielded **585.49 requests/s**, **p50 19.59 ms**, **p99 40.61 ms**; 8,815 requests in 15.06 s.
3. Verified Tailscale showed an active **direct** peer route (`direct 192.168.0.1:21388`), not an observed DERP route.
4. A preliminary single-run comparison yielded **546.90 requests/s LAN** versus **524.58 requests/s Tailscale**; these runs were not included in the later six-run summary.
5. Used `pidstat -u -p ALL 1` during Tailscale traffic. Selected busy samples showed `tailscaled` ~115–119% CPU, server ~10–13%, journald ~26–28%, rsyslogd ~24–27%, and Wi-Fi-related kernel worker ~25–26%. These are per-single-core-normalized percentages and selected samples, not six-run group averages.
6. Ran a six-trial comparison on the same V1 server: **LAN → Tailscale → Tailscale → LAN → LAN → Tailscale**, 2 wrk threads, 25 connections, 5 s warm-up, 15 s measured, 2 s timeout, 10 s cooldown.
7. Summarized the resulting run-level throughput and latency values using `summarize.py`.

## Main results

| Metric | Direct LAN (n=3) | Tailscale (n=3) |
|---|---:|---:|
| Mean throughput (requests/s) | **730.00** | **450.24** |
| Throughput sample SD | 5.96 | 11.95 |
| Throughput range | 724.24–736.14 | 437.38–461.01 |
| Mean run-level p50 (ms) | 15.45 | 25.61 |
| Mean run-level p99 (ms) | 31.89 | 47.78 |

Direct LAN delivered **62.1% more requests/s relative to Tailscale** in the six-trial batch. The non-overlapping throughput ranges strengthen the observation **for this batch**, without proving a universal performance ratio.

Other measurements: During the earlier monitored Tailscale load, `vmstat` showed roughly 44–50% machine-wide CPU idle, large context-switch activity and no observed swapping; server RSS stayed near 2,816 KiB. Sampled listening socket queue was empty (`Recv-Q 0`, `Send-Q 4096`). These observations do not reveal microsecond-level queue spikes or a definitive throughput root cause.

## Interpretation — provisional, to verify

- The choice of network route produced a repeatable **end-to-end** performance difference under the tested load.
- High `tailscaled` CPU correlates with tunneled traffic, but it does **not** prove Tailscale encryption alone caused the throughput gap; Wi-Fi, packet processing, scheduling and logging remain alternative or contributing explanations.
- The C++ process itself did not approach a full CPU core in sampled Tailscale runs; neither application CPU saturation nor memory exhaustion was demonstrated.
- Sequential blocking I/O remains a **separate architectural weakness**: a slow accepted client may delay unrelated clients. The 1 s receive timeout bounds each `recv()` wait but does not provide concurrency.
- A complete throughput-collapse/saturation point and the metric changing **before** user-visible degradation were not identified by these route comparison trials alone.

## Original Day 5 engineering questions

Answer these personally from evidence. Where evidence is absent, write **not measured** and propose a test rather than guessing.

1. **What failed first (throughput, latency, errors, memory or CPU headroom)?**  
   Current evidence: LAN/Tailscale throughput and latency diverged; no confirmed errors, memory pressure or application CPU saturation. A first failure *as load increases* was not established by fixed-c25 trials.  
   **My answer:** TODO.

2. **Which metric changed before the user-visible degradation?**  
   Measurements were not aligned to a graded workload transition sufficiently to determine temporal precedence.  
   **My answer / future instrumentation:** TODO.

3. **Which alternative bottlenecks could cause the same symptoms?**  
   Candidates: overlay processing, home Wi-Fi interference, TCP connection churn, stdout/journald/rsyslog logging, single-threaded blocking, client-side load generator behavior.  
   **Which is most plausible and why?** TODO.

4. **How would I distinguish CPU saturation from lock contention?**  
   V1 is single-threaded; do not assert a mutex contention bug without locks. Under a multithreaded design, inspect per-thread CPU/run queues and lock wait events.  
   **My answer:** TODO.

5. **How would I distinguish application from kernel/network bottlenecks?**  
   Current evidence: LAN-vs-Tailscale and process CPU measurements. Next: synchronized route-specific process profiles, network/softirq indicators and controlled configuration changes.  
   **My answer:** TODO.

6. **Is one CPU core saturated while others are mostly idle?**  
   `tailscaled` exceeds 100% of a single core's worth in some samples, but that does not mean any individual core was pinned. Per-core busy distribution under matched load was not captured.  
   **My answer / measurement:** TODO.

7. **If so, what in the architecture causes imbalance?**  
   The server has one request-processing thread, but sampled utilization does not show this thread saturating a CPU core.  
   **My answer:** TODO.

8. **Are connections waiting in the backlog?**  
   Sampled listening socket: `LISTEN 0 4096` on port 8080. No persistent backlog occupancy demonstrated at those moments.  
   **My answer / caveat:** TODO.

9. **Do context switches rise as concurrency increases?**  
   Tens of thousands per second were seen during c25 Tailscale traffic, but a matched concurrency sweep was not collected in this environment.  
   **My answer / proposed test:** TODO.

10. **Which measurement would most strongly disprove my hypothesis?**  
    For H-NET: repeated controlled direct-LAN/Tailscale runs showing equivalent throughput, or a configuration-independent application limit under matched conditions.  
    **My falsification criterion:** TODO.

## Reflection — complete in my own words

### What surprised me?
TODO — state the expectation I had *before* the test, then the contradictory measurement.

### What assumption did I revise?
TODO — explain exactly which evidence changed the assumption. Avoid claiming a cause that wasn't isolated.

### What did I misunderstand about CPU percentages?
TODO — explain why `pidstat` can report `tailscaled` >100% on a four-core Pi and why this does not prove core saturation.

### What went wrong with the first measurement design?
TODO — consider comparing different locations, relying on a single trial, and the absence of synchronized route-specific process metrics.

### What would I do differently next time?
TODO — name one concrete change in procedure, one metric to capture, and the result that would refute the current hypothesis.

## Day 5 decision boundary

**Do not silently change the V1 server in this experiment.** Document the current diagnosis in `docs/design-decisions.md`. Day 6 will compare one bounded-concurrency architecture against the same V1 binary on the same direct-LAN route, measuring slow-client isolation, latency, throughput, error behavior and resource bounds. A separate logging-on/off experiment may be useful, but must be versioned and measured independently.

## Evidence references

- `experiments/003-bottleneck-investigation/methodology.md`
- `results/raw/day05/runs/20261010_101930/results.csv` and six corresponding raw `wrk` files
- `results/raw/day05/runs/{compare-networks.sh,summarize.py}`
- `results/raw/day05/home-network/` system monitoring logs
- `results/raw/day05/` build and service metadata
- `docs/design-decisions.md` (create/maintain separately)

## Applications (original roadmap requirement)

- **Systems/infra CV:** TODO — select verified claims about Linux process supervision, system metrics, recovery and experiment design.
- **C++/trading CV:** TODO — select verified claims about POSIX sockets, blocking I/O, concurrency trade-offs, latency measurements and reproducible benchmarking.
- **Accuracy check:** Do not claim a throughput optimization, reduced p99, or worker-pool implementation before it has actually been measured or built.
Hypothesis A — Logging overhead: Request-by-request output adds synchronous work and logging-system overhead.
Hypothesis B — Sequential request handling: Even with a one-second receive timeout, processing one client at a time may limit throughput and increase latency.
Hypothesis C — Network overhead: The transport path significantly affects measured throughput independently of application changes.
