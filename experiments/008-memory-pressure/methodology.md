
# Experiment 008 — Memory Pressure, OOM, and Resource Isolation

**Project:** Resilient Pi  
**Date:** 2026-10-15  
**Milestone:** Day 10 — Memory Pressure and OOM  
**Platform:** Raspberry Pi 4B, Ubuntu Server 24.04 ARM64

## 1. Objective

Investigate how Linux manages memory under increasing pressure and how resource boundaries affect application reliability.

The experiment addresses five questions:

1. How do virtual memory, resident memory, and cgroup-accounted memory differ?
2. What happens when a process exceeds the cgroup `MemoryHigh` threshold?
3. What happens when memory consumption exceeds `MemoryMax`?
4. Can an OOM event be isolated without terminating unrelated services?
5. Can the C++ HTTP server operate under explicit memory limits?

The experiment prioritizes controlled failure injection and reproducible evidence over system-wide resource exhaustion.

## 2. Experimental Environment

| Component | Configuration |
|---|---|
| Hardware | Raspberry Pi 4 Model B |
| CPU | Four ARM cores |
| Physical memory | Approximately 8 GB, 7.6 GiB reported |
| Operating system | Ubuntu Server 24.04 ARM64 |
| Kernel | Linux 6.8.0-1065-raspi |
| Cgroup version | v2 |
| Memory controller | Available |
| Swap | Disabled |
| Service manager | systemd |
| HTTP server | C++20, single-threaded |
| Database | PostgreSQL 16 |
| Memory stress | Python page-touch allocation |

The Raspberry Pi also hosted Prometheus, Grafana, Node Exporter, SSH, and VS Code remote processes.

The original `resilient-pi.service` and PostgreSQL service were kept outside the experimental memory cgroups.

## 3. Safety and Isolation Strategy

Deliberately exhausting all available physical memory could destabilize the operating system, interrupt SSH, or affect PostgreSQL.

Instead, memory pressure was confined to systemd-managed transient units.

Two cgroup controls were used:

- `MemoryHigh`: reclaim and throttling boundary
- `MemoryMax`: hard memory boundary

Swap was disabled for the experimental units using:

```ini
MemorySwapMax=0
```

The system itself also had no swap configured.

All memory-stress experiments used dedicated systemd units rather than modifying the production HTTP service.

The OOM experiment was limited to 384 MiB, substantially below the host's total physical memory.

## 4. Baseline Inspection

### Procedure

Inspect host memory and memory-pressure state using:

```bash
free -h
swapon --show
cat /proc/meminfo
cat /proc/pressure/memory
vmstat 1 5
```

Verify cgroup support:

```bash
stat -fc %T /sys/fs/cgroup
cat /sys/fs/cgroup/cgroup.controllers
```

Inspect the current HTTP service limits:

```bash
systemctl show resilient-pi \
  -p MemoryCurrent \
  -p MemoryHigh \
  -p MemoryMax \
  -p MemorySwapMax \
  -p OOMPolicy
```

### Observations

| Measurement | Value |
|---|---|
| Total memory | 7.6 GiB |
| Used memory | Approximately 2.0 GiB |
| Available memory | Approximately 5.6 GiB |
| Swap | 0 B |
| `MemTotal` | 7,989,308 kB |
| `MemAvailable` | 5,852,364 kB |
| Cached memory | 3,842,212 kB |
| Anonymous pages | 1,592,200 kB |
| Reclaimable slab | 169,084 kB |

Memory PSI:

```text
some avg10=0.00 avg60=0.00 avg300=0.00
full avg10=0.00 avg60=0.00 avg300=0.00
```

Cgroup verification:

```text
cgroup2fs
```

The memory controller was available.

The HTTP service initially reported:

```text
MemoryHigh=infinity
MemoryMax=infinity
MemorySwapMax=infinity
```

### Interpretation

The host had substantial available memory and no measurable recent memory-pressure stalls.

The difference between free and available memory demonstrates why unused RAM alone is not an appropriate measure of memory health.

Linux uses memory for caches that can often be reclaimed when applications need additional capacity.

## 5. Memory Allocation Method

A Python probe was created at:

```text
experiments/008-memory-pressure/allocate.py
```

The probe:

1. Allocates a configurable byte array.
2. Writes to each allocated page.
3. Reports process memory information.
4. Keeps the allocation alive for approximately 20 seconds.

Page touching was used to make physical memory demand observable rather than relying on a virtual-memory reservation alone.

The initial probe used a 192 MiB allocation.

Later experiments requested 300 MiB and 512 MiB.

### Measurement sources

Process memory:

```text
/proc/<pid>/status
```

Cgroup memory:

```text
memory.current
memory.peak
memory.events
memory.pressure
```

Systemd accounting:

```bash
systemctl show <unit> \
  -p MemoryCurrent \
  -p MemoryPeak \
  -p MemoryHigh \
  -p MemoryMax \
  -p Result
```

Kernel OOM evidence:

```bash
sudo journalctl -k -b
```

These sources measure related but different aspects of memory consumption.

## 6. Experiment A — Normal Allocation

### Hypothesis

A process allocating less memory than both `MemoryHigh` and `MemoryMax` should complete successfully without OOM.

### Configuration

| Parameter | Value |
|---|---|
| Requested allocation | 192 MiB |
| `MemoryHigh` | 256 MiB |
| `MemoryMax` | 384 MiB |
| `MemorySwapMax` | 0 |

### Execution

```bash
sudo systemd-run \
  --unit=resilient-pi-memory-probe \
  --wait \
  --collect \
  -p MemoryAccounting=yes \
  -p MemoryHigh=256M \
  -p MemoryMax=384M \
  -p MemorySwapMax=0 \
  /usr/bin/python3 \
  /home/hojune/projects/resilient-pi/experiments/008-memory-pressure/allocate.py
```

### Observations

The Python program reported:

```text
Touched 192 MiB of memory
VmSize: 211332 kB
VmRSS:  206160 kB
VmHWM:  206160 kB
```

The successful repeat reported:

```text
Service runtime: 20.552s
Memory peak: 196.3M
```

Live cgroup measurements included:

```text
MemoryCurrent=205668352
MemoryPeak=205852672
MemoryHigh=268435456
MemoryMax=402653184
```

Memory events:

```text
low 0
high 0
max 0
oom 0
oom_kill 0
oom_group_kill 0
```

### Interpretation

The allocation was backed by resident pages and accounted to the experimental cgroup.

No memory-high enforcement or OOM events occurred.

The process completed successfully.

### Measurement anomaly

The first systemd invocation reported an unexpectedly low memory peak of 652 KiB despite the Python program reporting approximately 201 MiB RSS.

A subsequent invocation and live cgroup inspection showed memory usage consistent with the intended allocation.

The discrepancy was not independently diagnosed. It should not be treated as evidence that the first allocation was absent.

## 7. Experiment B — MemoryHigh Enforcement

### Hypothesis

Exceeding `MemoryHigh` should produce reclaim pressure and allocation stalls without necessarily triggering OOM.

### Configuration

| Parameter | Value |
|---|---|
| Requested allocation | 300 MiB |
| `MemoryHigh` | 256 MiB |
| `MemoryMax` | 384 MiB |
| `MemorySwapMax` | 0 |

### Execution

```bash
sudo systemd-run \
  --unit=resilient-pi-memory-high \
  --collect \
  -p MemoryAccounting=yes \
  -p MemoryHigh=256M \
  -p MemoryMax=384M \
  -p MemorySwapMax=0 \
  /usr/bin/python3 \
  /home/hojune/projects/resilient-pi/experiments/008-memory-pressure/allocate.py \
  300
```

### Observations

During execution:

```text
MemoryCurrent=280285184
MemoryPeak=280793088
MemoryHigh=268435456
MemoryMax=402653184
```

Memory events:

```text
low 0
high 1562
max 0
oom 0
oom_kill 0
oom_group_kill 0
```

Memory PSI:

```text
some avg10=40.19 avg60=8.97 avg300=1.94
full avg10=40.19 avg60=8.97 avg300=1.94
```

The process remained running rather than completing within the expected time.

It was manually stopped at 07:38:39, having started at 07:31:15.

Final journal output reported:

```text
Deactivated successfully
Consumed 1.587s CPU time
282.2M memory peak
0B memory swap peak
```

The successful systemd stop does not mean the allocation completed successfully.

### Interpretation

The cgroup crossed its `memory.high` threshold.

The kernel recorded 1,562 high-limit events and substantial memory-pressure stalls.

The process made insufficient progress to complete normally during the observed period and required manual intervention.

### Conclusion

`MemoryHigh` is a memory-pressure control, not a guaranteed termination boundary.

A process may remain alive while experiencing severe reclaim pressure and prolonged delays.

## 8. Experiment C — Cgroup-Local OOM

### Hypothesis

A memory allocation larger than the cgroup hard limit can trigger a cgroup-local OOM, terminating the constrained workload without exhausting host memory.

### Configuration

| Parameter | Value |
|---|---|
| Requested allocation | 512 MiB |
| `MemoryHigh` | Infinity |
| `MemoryMax` | 384 MiB |
| `MemorySwapMax` | 0 |
| `OOMPolicy` | Stop |

### Execution

```bash
sudo systemd-run \
  --unit=resilient-pi-memory-oom \
  -p MemoryAccounting=yes \
  -p MemoryHigh=infinity \
  -p MemoryMax=384M \
  -p MemorySwapMax=0 \
  -p OOMPolicy=stop \
  /usr/bin/python3 \
  /home/hojune/projects/resilient-pi/experiments/008-memory-pressure/allocate.py \
  512
```

### Observations

Systemd reported:

```text
A process of this unit has been killed by the OOM killer.
Main process exited, code=killed, status=9/KILL
Failed with result 'oom-kill'.
```

The kernel reported:

```text
oom-kill:constraint=CONSTRAINT_MEMCG
oom_memcg=/system.slice/resilient-pi-memory-oom.service
task=python3,pid=35018
```

Additional kernel information:

```text
Memory cgroup out of memory:
Killed process 35018 (python3)
total-vm:539012kB
anon-rss:392320kB
file-rss:5888kB
```

Systemd later reported:

```text
Result=oom-kill
ExecMainCode=2
ExecMainStatus=9
```

### Interpretation

The allocation exceeded the memory available within its cgroup.

The kernel selected the Python allocation process as the OOM victim.

`CONSTRAINT_MEMCG` identifies the memory-cgroup limit as the relevant OOM constraint.

The process terminated through SIGKILL.

The transient cgroup was removed after termination, so its `memory.events` file was unavailable when inspected afterward.

### Conclusion

The configured cgroup hard limit successfully bounded the experimental workload.

Exceeding the limit resulted in a contained failure rather than a host-wide memory exhaustion event.

## 9. Experiment D — Service Failure Isolation

### Hypothesis

The memory-cgroup OOM should not terminate the independent C++ HTTP server or PostgreSQL.

### Procedure

After the OOM event:

```bash
systemctl is-active resilient-pi
systemctl is-active postgresql@16-main

curl -i --max-time 5 \
  http://127.0.0.1:8080/

free -h
```

### Observations

```text
resilient-pi: active
postgresql@16-main: active
```

The HTTP server returned:

```text
HTTP/1.1 200 OK
Content-Length: 3

OK
```

Available host memory after the OOM was approximately 5.3 GiB, with a later evidence snapshot showing approximately 5.6 GiB available.

### Conclusion

The experimental OOM was isolated from the original HTTP server and PostgreSQL in the observed test.

The test demonstrates failure isolation under this configuration, not a universal guarantee against all forms of shared-resource interference.

## 10. Experiment E — Memory-Limited C++ Server

### Objective

Verify that a separate instance of the C++ server can operate under finite systemd memory limits.

### Implementation

The original V1 server was copied to:

```text
experiments/008-memory-pressure/limited_server.cpp
```

The experimental copy uses TCP port 8083 instead of port 8080.

It was compiled as:

```text
build/day10/limited-server
```

### Configuration

| Parameter | Value |
|---|---|
| `MemoryHigh` | 48 MiB |
| `MemoryMax` | 64 MiB |
| `MemorySwapMax` | 0 |

### Execution

```bash
sudo systemd-run \
  --unit=resilient-pi-limited \
  -p MemoryAccounting=yes \
  -p MemoryHigh=48M \
  -p MemoryMax=64M \
  -p MemorySwapMax=0 \
  /home/hojune/projects/resilient-pi/build/day10/limited-server
```

### Observations

```text
MemoryCurrent=282624
MemoryHigh=50331648
MemoryMax=67108864
ActiveState=active
```

The experimental server responded:

```text
HTTP/1.1 200 OK

OK
```

After stopping the experimental service, both the original C++ server and PostgreSQL remained active.

### Conclusion

The experimental C++ server operated successfully under the configured memory limits for the observed HTTP request.

This does not establish whether the limits provide sufficient headroom for sustained workloads or higher concurrency.

## 11. Measurement Limitations

- No system-wide OOM was attempted.
- The first allocation experiment showed a systemd memory-peak reporting anomaly.
- The `MemoryHigh` workload was manually stopped rather than completing normally.
- Memory pressure was not correlated with a simultaneous HTTP load benchmark.
- No complete time-aligned before/after memory graph was recorded.
- A cgroup-level OOM was demonstrated, but cgroup event counters were not captured before the cgroup disappeared.
- The 64 MiB C++ limit was tested with a single successful HTTP request, not a sustained load.
- No memory leak was demonstrated or investigated.
- Swap was disabled, so swapping behavior was not experimentally evaluated.
- The OOM victim was observed within a single-purpose cgroup; victim selection among multiple eligible processes was not tested.

## 12. Evidence

```text
experiments/008-memory-pressure/allocate.py
experiments/008-memory-pressure/limited_server.cpp

results/raw/day10/baseline-memory.txt
results/raw/day10/oom-isolation.txt
results/raw/day10/memory-high-journal.txt
results/raw/day10/oom-service-journal.txt
results/raw/day10/oom-kernel.txt
```

The journal files should be verified as present before the experiment is committed.

## 13. Conclusion

The experiment demonstrated three distinct memory-management behaviors:

1. Allocations below the configured thresholds can complete normally.
2. Exceeding `MemoryHigh` can produce significant reclaim pressure and prolonged stalls without OOM.
3. Exceeding `MemoryMax` can trigger a cgroup-local OOM that terminates the offending process while independent services continue operating.

The results support using application-level resource bounds together with operating-system resource isolation.

For production deployments, OOM termination should be treated as a last-resort containment mechanism rather than the primary overload-management strategy.
