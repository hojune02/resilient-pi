# Resilient Pi — measured reliability on a Raspberry Pi

> A C++/Linux HTTP server testbed for understanding process recovery, bounded concurrency, PostgreSQL durability, observability, and resource/failure isolation through reproducible experiments.

**Release:** v0.1.0 candidate (not tagged in this archive). **Status:** learning/portfolio testbed, **not** a production-ready HTTP stack. All performance figures below describe a specific workload and environment. No claims of production scale or zero data loss.

## 1. Thesis

A working server is not the same as a resilient server. I built small C++ servers on a Raspberry Pi, exercised real failure modes, collected Linux and client-side measurements, and retained unsuccessful experiments instead of optimizing for impressive-looking numbers.

## 2. Architecture

```text
Arch Linux / Mac client --TCP--> Raspberry Pi 4B, Ubuntu Server 24.04 (ARM64)
                                   |
  :8080 V1 single-threaded POSIX socket server <-- systemd Restart=on-failure
  :8081 PostgreSQL-backed proof of concept (manually launched)
  :8082 Observable C++ server (health, ready, counter, metrics)
  :8085 Idempotency-key experimental C++ server (separate, manual)
                                   | libpq local Unix socket
                              PostgreSQL 16
  Prometheus :9090 <-- /metrics    Node Exporter :9100
       | 
  Grafana :3000 (via SSH tunnel)
```

The processes on ports 8081, 8082 and 8085 are **not** all supervised by the `resilient-pi.service` unit. Direct-Ethernet load tests primarily exercised port 8080. These experiments do not represent one integrated production deployment. `V2` on `server/worker_pool.cpp` is a separate four-worker experiment; it is not the live default service.

## 3. Hardware constraints

- Raspberry Pi 4B, four Cortex-A72 cores; approximately **7.6 GiB usable RAM** reported; microSD storage, Ubuntu Server 24.04 ARM64.
- Separate Arch Linux client connected via direct Ethernet `10.42.0.1 -> 10.42.0.2`; physical link was observed at 100 Mb/s. Earlier MacBook/campus-network benchmarks used a different route.
- No swap at the time of Day 10 testing. Shared host processes included PostgreSQL, Grafana, Prometheus and VS Code Remote.
- Kernel and systemd scheduling, I/O, and telemetry overhead are part of the measured system. It is not a controlled hardware laboratory.

## 4. C++ design and boundaries

| Variant | Source | Purpose | Important limitation |
|---|---|---|---|
| V1 | `server/main.cpp` | Baseline sequential accept/recv/send/close, systemd recovery | Single recv, per-request logging; no database |
| V2 | `server/worker_pool.cpp` | One acceptor, four workers, 64-entry bounded application queue | Queue-full closes without HTTP response; synchronized per-request logging |
| Persistent | `server/persistent_server.cpp` | PostgreSQL counter and reconnect tests | Single shared libpq connection; synchronous I/O |
| Observable | `server/observable_server.cpp` | `/health`, `/ready`, `/counter`, `/metrics` | Simple HTTP parsing, single thread; db readiness ≠ process liveness |
| Idempotent | `server/idempotent_server.cpp` | HTTP `Idempotency-Key` and atomic deduplication experiment | Port 8085; independently deployed and not load/soak validated |

The idempotent path uses PostgreSQL's unique request key, a transaction that claims the key, increments once, and persists the response before COMMIT. A repeated key returns the stored result. The F05 SQL demonstration is separately recorded. Request-key retention/expiry, full HTTP protocol compliance, access control and robust operational deployment remain out of scope.

## 5. Benchmark methodology

- `wrk` with `-t2`, 15-second measured intervals, generally 5-second warm-ups and three repetitions per concurrency point. Check individual `methodology.md` for the route/timeouts used; they **vary across experiments**.
- Report requests/sec, p50/p99 latency, read/socket errors, timeouts, and when available process/host metrics (`pidstat`, `mpstat`, `vmstat`, thermal and frequency sampling).
- Two fundamentally different datasets: Day 4 / Day 6 campus-route load tests and later direct-Ethernet tests. **Never pool them as one comparable benchmark.**
- The Day 6 direct-Ethernet aggregate values discussed in the engineering journal are not fully reproducible from raw `wrk` files in this ZIP. This release reports the complete campus-route Day 6 dataset and treats direct-Ethernet follow-up as partially archived evidence.

## 6. Baseline results (Day 4)

The Day 4 campus-route `results/raw/day04/baseline.csv` contains three trials per listed concurrency. For instance at `c=10`, observed rates were **501.81, 478.98, 476.58 req/s** (mean approximately **485.8 req/s**). This is a different test setup from Day 6's V1-versus-V2 run and must not be interpreted as a before/after improvement.

See `experiments/002-performance-baseline/methodology.md`, `results/raw/day04/`, and `results/figures/`.

## 7. Concurrency comparison (Day 6)

**Archived campus-route run (three trials per point):**

| Connections | V1 avg req/s | V2 avg req/s | V2 change |
|---:|---:|---:|---:|
| 1 | 63.81 | 62.11 | -2.7% |
| 10 | 149.41 | 137.45 | -8.0% |
| 25 | 152.62 | 133.51 | -12.5% |
| 50 | 167.44 | 143.16 | -14.5% |
| 100 | 198.50 | 123.58 | -37.7% |

Source: `results/raw/day06/v1/results.csv`, `results/raw/day06/v2/results.csv`, and `experiments/004-concurrency-comparison/comparison.md`.

V2 **did not** improve throughput in this workload. The worker pool does bound its *application* queue and can isolate clients blocked in receive calls, but rejection may manifest as a socket error. The cause of regression is not proven; serialized logging, queue contention and network variability remain candidates. Higher later direct-Ethernet numbers are not claimed here as a replicable release result.

## 8. CPU saturation experiment (Day 9)

Arch Linux `wrk -t2 -c50 -d15s --latency --timeout 5s` against direct Ethernet while `stress-ng` added CPU work:

| External CPU workers | Req/s | p50 ms | p99 ms |
|---:|---:|---:|---:|
| 0 baseline | 2753.95 | 5.48 | 371.64 |
| 1 | 2566.94 | 6.78 | 446.39 |
| 2 | 2201.67 | 10.78 | 269.48 |
| 3 repeat | 1929.54 | 12.31 | 273.89 |
| 4 verification | 2092.75 | 11.02 | 223.32 |
| 0 later | 2841.86 | 5.09 | 421.96 |

CPU4 verification had ~99.9% mean host utilization in its aligned window. No thermal throttle was observed (`get_throttled=0x0`). Throughput decreased ~24% vs first baseline and median latency approximately doubled; **p99 was non-monotonic**. Primarily single trials and shared-system drift limit causal claims. See `experiments/007-cpu-saturation/findings.md` and `results/raw/day09/`.

## 9. Memory and cgroup OOM (Day 10)

- Baseline: **7.6 GiB total / ~5.6 GiB available**, zero swap, PSI averages at zero.
- 192 MiB page-touched allocation completed inside a 384 MiB cgroup hard limit.
- With `MemoryHigh=256M` and 300 MiB requested, **1,562 `memory.events high` events** and **40.19% memory PSI `some avg10`** were observed. Process failed to complete during observation and was **manually stopped**.
- With `MemoryMax=384M`, a 512 MiB request produced `CONSTRAINT_MEMCG`, systemd `oom-kill`, SIGKILL to the Python worker. The original HTTP service and PostgreSQL remained active.
- A separate port-8083 HTTP copy returned 200 under `MemoryMax=64M` for one request; this **does not** prove 64 MiB is a suitable sustained-load limit.

See `experiments/008-memory-pressure/`, `results/raw/day10/`, `journal/day10.md`.

## 10. Multi-failure experiments (Day 11)

| Test | Injected fault | Observed result | Caveat |
|---|---|---|---|
| F01 | SIGKILL HTTP V1 | 11 failed polls; 2.315 s fault-command -> first HTTP 200 | Sampled restart, not exact downtime |
| F02 | Stop PostgreSQL | 60 HTTP 503 readiness checks; 18.615 s fault command -> readiness recovery | Includes deliberately maintained downtime |
| F03 | nftables drop Arch -> Pi HTTP | 12 ~1s timeouts; 14.685 s fault procedure -> first HTTP 200 | Timeout and intentional timer period included |
| F04 | Terminate one PostgreSQL backend | One HTTP 503; 429 ms to first recovered readiness | DB stayed available; not a sustained network partition |
| F05 | Simulate lost COMMIT acknowledgement | Naïve retry final counter 2; idempotent SQL scenario final counter 1, replayed result 1 | Scripted branching; not physical packet loss |

**Do not compare F01–F04 durations as equivalent pure MTTR:** fault hold time, request timeout, and observation resolution differ.

## 11. Incident reports

See [`docs/incidents.md`](docs/incidents.md) for evidence, caveats, and the failed-first-try F05 correction. Raw traces are in `results/raw/day11/`. The HTTP idempotency server is a later implementation; the incident report's description of the earlier SQL demonstration is historical, not a description of every subsequent code change.

## 12. What failed, and what changed

1. **Simple crash-recovery tests** required explicit, aligned detection/recovery definitions; a systemd restart does not imply zero failed requests.
2. **V2 concurrency** added queue bounds but was slower on the archived campus route. We retained the result and kept performance optimization unproven.
3. **CPU pressure** lowered throughput without evidence of thermal throttling; scheduling contention is a plausible contributing mechanism, not isolated root cause.
4. **`MemoryHigh`** did not terminate the allocation and produced prolonged stalls; a hard limit triggers local OOM but isn't a normal overload policy.
5. **F04 transport mismatch:** initial database connection used a Unix socket while reconnect used TCP. Socket inspection (`ss`) exposed the discrepancy; the disposable experiment aligned both strings.
6. **F05 initial SQL** stored placeholder `-1` despite only one increment. Splitting data modifications into sequential statements in a transaction preserved the correct reply.
7. **Retries** can duplicate committed writes when the acknowledgement is lost; unique request IDs coupled with atomic outcomes are necessary for controlled replay.

## 13. Reproduction (on a Pi or compatible Linux host)

**Prerequisites:** C++20 compiler, `libpq-dev`, Python 3, PostgreSQL 16 (or compatible), `psql`; for load tests `wrk`, `sysstat`, `stress-ng`. The supplied archive contains the code and evidence, **not** a provisioned operating system or complete exported service/unit/monitoring setup.

```bash
sudo apt-get update
sudo apt-get install -y g++ libpq-dev postgresql-client python3
cd ~/projects/resilient-pi
mkdir -p build
# Baseline: port 8080; stop resilient-pi.service before running this manually.
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic server/main.cpp -o build/v1
# Database-backed: server port 8082, requires PostgreSQL role/database/schema.
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -I"$(pg_config --includedir)" server/observable_server.cpp -lpq -o build/observable
# Idempotent test server: loopback port 8085.
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -I"$(pg_config --includedir)" server/idempotent_server.cpp -lpq -o build/idempotent
```

For a fresh installation, create the database and role with local PostgreSQL administration first. **Review `database/schema.sql` before applying**; the existing DB may already contain persistent rows. Then apply `migrations/001_http_idempotency.sql` as the application database owner. Database connection defaults assume a local OS/DB user `hojune` with peer authentication; for another setup provide `RESILIENT_DB_CONNINFO` securely via your shell or service environment, **without committing credentials**.

```bash
psql -v ON_ERROR_STOP=1 -d resilient_pi -f migrations/001_http_idempotency.sql
./build/idempotent                # foreground, terminal A
python3 tests/test_idempotency.py # terminal B; modifies requests counter!
```

The integration test deliberately increments the **real `requests` counter** by three, and therefore should be run only against a disposable/testing database or after taking a backup. The server includes a test-only `RESILIENT_TEST_DROP_AFTER_COMMIT_ONCE` fault hook; never enable it in production.

**Example idempotency request:**

```bash
curl -i -X POST -H 'Idempotency-Key: trial-2026-001' -H 'Content-Length: 0' http://127.0.0.1:8085/counter/increment
# Repeat with the same key: same response and no additional increment.
```

For crash, CPU and memory experiments, use the archived scripts only after reading their safety settings. Do **not** run load tests against a shared database or alter globally managed Tailscale firewall rules. Inspect `results/raw/` for original evidence and the methodology docs for differences in load-generator path.

## 14. Open questions / next work

- Validate idempotency under **true parallel server-side** database transactions, crash injection after COMMIT, retained key expiry, and conflicting payload fingerprints.
- Improve request parsing, timeouts, capacity limits, and lifecycle management before exposing the HTTP server outside localhost.
- Complete matched/direct-Ethernet V1/V2 datasets, archive all raw `wrk` output including socket errors, and randomize A/B order with workload isolation.
- Quantify logging/journald impact, queue depth, TCP state, CPU scheduling delay, and sustained throughput stability.
- Establish production-sized memory budgets after realistic soak tests, not from one HTTP request.
- Supervise the database-backed application, alert on readiness failures, and verify PostgreSQL transaction recovery after ambiguous COMMIT.

## Three evidence-supported claims

1. Under the controlled process-failure test, systemd restarted V1 and the client observed service recovery; 20 earlier crash trials in `results/raw/crash_recovery_50ms.csv` had **median 2,171 ms** measured recovery.
2. In the archived Day 6 campus-route comparison, the four-worker bounded-queue server was **slower** than the single-threaded baseline at all five tested concurrencies. Bounded concurrency is not a throughput guarantee.
3. A memory-cgroup hard limit contained a synthetic allocation OOM to its unit while the unrelated HTTP service and PostgreSQL remained active; independent multi-failure tests distinguish liveness, readiness and connectivity.

**One-sentence description:** “Built a Raspberry Pi C++/Linux reliability testbed and used instrumented failure injection and network benchmarks to characterize restart, concurrency, memory/OOM, PostgreSQL, and retry behavior.”

## Engineering interview: two-minute debugging story

**Symptom:** A disposable PostgreSQL-backed C++ service passed `/ready`, but the supposed TCP connection wasn't visible in `ss -tnp`. **Hypothesis check:** `ss -xap` showed its actual open fd was a PostgreSQL Unix socket. **Root cause:** the startup connection string lacked `host=127.0.0.1`, while reconnection had TCP settings. **Fix:** unify the two connection strings and verify an `ESTAB 127.0.0.1:<ephemeral> -> 127.0.0.1:5432` socket before fault injection. **Lesson:** end-to-end green responses do not validate an internal assumption about transport; observe the system at the layer where the hypothesis lives.

See [`docs/release-audit.md`](docs/release-audit.md) for claims deliberately excluded from this release.
