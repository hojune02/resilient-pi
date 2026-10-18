# Day 12 — Release preparation and evidence audit

**Date:** 2026-10-18  
**Scope:** package existing code and experiments, correct release claims, prepare `v0.1.0` candidate.

## Delivered

- Replaced empty README with thesis, architecture, hardware, C++ variants, methodology, baseline, concurrency, CPU, memory/OOM, multi-failure, incident reports, failed assumptions, reproduction, and open questions.
- Added release audit and release notes.
- Compiled `server/observable_server.cpp`, `server/idempotent_server.cpp`, and `server/worker_pool.cpp` on an external Linux build host with C++20 and warnings enabled.
- Checked Python test syntax. Target-Pi runtime/integration validation remains pending.
- No Git tag was created or pushed. Preserve raw evidence before final tag.

## Ten engineering questions

### 1. Three strongest supported claims

1. In 20 archived 50-ms-polling crash recovery trials, all recorded success with median recovery 2,171 ms; the separate F01 trial had 11 failed probes and HTTP availability returned 2.315 seconds after SIGKILL.
2. Day 6 archived campus-network V1/V2 comparison shows bounded four-worker V2 did not outperform the sequential baseline at any tested concurrency; the design improves queue bounding but not automatically throughput.
3. Day 10 synthetic cgroup-local OOM killed only the restricted allocation process while the independent HTTP server and PostgreSQL remained active; Day 11 additionally distinguishes dependency readiness from process liveness.

### 2. Tempting but unsupported claim

“V2 scales better than V1 under high load.” The fully archived campus-route data shows the opposite; direct-Ethernet follow-up has unresolved variability/errors and insufficient raw archival in this ZIP.

### 3. Most important trade-off

A bounded worker queue improves overload predictability and isolates blocked clients but adds synchronization and overhead, may reject clients by closing sockets without HTTP responses, and is not necessarily faster.

### 4. Most incorrect assumption

A healthy `/ready` on the F04 disposable server was initially taken as confirmation of a TCP PostgreSQL connection. `ss` showed it had actually connected via a Unix socket because startup and reconnection conninfo differed.

### 5. Measurement that changed the design narrative

The Day 6 V2 throughput regression; also Day 10 showed `MemoryHigh` severe stalls rather than quick termination, pushing the design toward application admission control plus hard cgroup containment.

### 6. Built vs configured

**Built/implemented:** POSIX-socket C++ server variants, a bounded worker pool, PostgreSQL/libpq operations and reconnect path, metrics instrumentation, idempotency transaction prototype, benchmark and probe scripts, incident experiments. **Configured:** Linux, systemd service, PostgreSQL, Tailscale, Prometheus, Node Exporter, Grafana. Not a claim of authoring those third-party systems.

### 7. Weakest benchmark methodology

Different network paths, sequential V1/V2 trial batches (order/time confounds), per-request logging overhead, noisy shared host, and incomplete archiving of later direct-Ethernet sweeps and socket errors. No randomized interleaving or long-duration variance analysis.

### 8. Skeptical engineer's next questions

What is the real bottleneck after removing request logging? How many requests fail at queue saturation? Does the idempotent endpoint remain correct across process restarts and concurrent multi-process writers? Can reproducible unit files and full network-route evidence be supplied? What happens to 99.9th-percentile latency under sustained pressure?

### 9. Accurate one-sentence description

Built a Raspberry Pi C++/Linux HTTP reliability testbed and characterized restart, bounded concurrency, PostgreSQL dependency failures, CPU pressure, cgroup OOM and retry correctness with instrumented experiments.

### 10. Best two-minute debugging story

The F04 transport mismatch: HTTP `/ready` returned 200, but the expected TCP `ss` connection was absent. Checked `/proc/PID/fd` and `ss -xap`; found a PostgreSQL Unix-domain socket. Grepped both PQconnectdb sites and found startup omitted `host=127.0.0.1` while reconnection specified TCP. Unified connection strings; verified the TCP ESTAB state and tested a targeted backend termination/reconnect. Lesson: verify assumptions at the OS boundary, not just through green HTTP status.

## Release decision

**CV-ready for a carefully qualified systems/reliability project**. `v0.1.0` should only be tagged after Pi-side integration tests, repository hygiene review, and confirming release content. The work is a testbed, not production reliability certification.
