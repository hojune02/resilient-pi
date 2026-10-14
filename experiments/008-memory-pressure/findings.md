
# Experiment 008 — Memory Pressure Findings

**Date:** 2026-10-15  
**System:** Resilient Pi  
**Status:** Core experiments completed

## 1. Executive Summary

Memory pressure and memory exhaustion are different failure conditions.

The experiments demonstrated that:

- A 192 MiB allocation completed normally within a 384 MiB memory-limited cgroup.
- A 300 MiB allocation crossing `MemoryHigh=256M` experienced severe memory pressure and did not complete normally during the observed interval.
- A 512 MiB allocation exceeding `MemoryMax=384M` triggered a cgroup-local OOM kill.
- The OOM victim was restricted to the experimental workload.
- The original C++ HTTP server and PostgreSQL remained operational.
- A disposable C++ HTTP server successfully handled a request under a 64 MiB hard memory limit.

The central finding is that memory limits can make resource-exhaustion failures more contained and predictable, but they do not eliminate latency, reclaim pressure, or the need for application-level resource bounds.

## 2. Experiment Comparison

| Experiment | Requested allocation | MemoryHigh | MemoryMax | Result |
|---|---:|---:|---:|---|
| Normal allocation | 192 MiB | 256 MiB | 384 MiB | Completed |
| Memory pressure | 300 MiB | 256 MiB | 384 MiB | Severe stalls; manually stopped |
| OOM | 512 MiB | Infinity | 384 MiB | Cgroup-local OOM kill |
| C++ service limit | Normal HTTP request | 48 MiB | 64 MiB | HTTP 200 |

These are four separate experiments. They should not be interpreted as one continuous memory-consumption timeline.

## 3. Finding F01 — Virtual Memory Is Not Resident Memory

The 192 MiB allocation probe reported:

```text
VmSize: 211332 kB
VmRSS:  206160 kB
VmHWM:  206160 kB
```

`VmSize` represents mapped virtual address space.

`VmRSS` represents resident physical pages attributed to the process.

`VmHWM` records the peak resident set size.

By touching each allocated page, the probe made the allocation materially resident.

The experiment confirmed that the memory allocation produced a substantial resident working set.

## 4. Finding F02 — Low Free Memory Does Not Necessarily Mean Pressure

Initial Linux memory information:

```text
MemTotal:     7989308 kB
MemFree:      2054168 kB
MemAvailable: 5852364 kB
Cached:       3842212 kB
```

Although only approximately 2 GiB was completely free, Linux estimated that around 5.6 GiB remained available to applications.

Linux uses otherwise idle physical memory for page caching and related purposes.

Therefore, `MemAvailable` and memory-pressure indicators are more useful for diagnosing memory scarcity than `MemFree` alone.

The baseline PSI readings were:

```text
some avg10=0.00
full avg10=0.00
```

No recent memory-pressure stalls were observed at baseline.

## 5. Finding F03 — MemoryHigh Can Cause Severe Stalls

Observed configuration:

```text
MemoryHigh=268435456
MemoryMax=402653184
```

Memory events:

```text
high 1562
max 0
oom 0
oom_kill 0
```

Memory PSI:

```text
some avg10=40.19
full avg10=40.19
```

The kernel repeatedly enforced the memory-high boundary.

The allocation process remained running for several minutes and required manual termination.

This demonstrates that a process can remain alive but make extremely slow progress under memory pressure.

A running process is not necessarily a progressing process.

### Operational implication

A service can become effectively unavailable due to memory reclaim or allocation stalls even before reaching an OOM condition.

Memory availability should therefore be monitored alongside latency and memory PSI.

## 6. Finding F04 — MemoryMax Can Produce a Contained OOM

The 512 MiB allocation was executed with:

```text
MemoryMax=384M
MemorySwapMax=0
```

Systemd reported:

```text
Result=oom-kill
ExecMainCode=2
ExecMainStatus=9
```

The kernel reported:

```text
constraint=CONSTRAINT_MEMCG
task=python3
pid=35018
```

The kernel terminated the Python allocation process with SIGKILL.

The failure occurred within the experimental memory cgroup.

### Operational implication

A memory hard limit can bound damage from an unexpectedly memory-intensive process.

However, that process may still be terminated abruptly, losing any in-memory state that has not been persisted.

## 7. Finding F05 — Independent Services Survived OOM

After the OOM:

```text
resilient-pi: active
postgresql@16-main: active
```

The HTTP server returned HTTP 200.

Host available memory remained above 5 GiB.

This provides evidence that the experimental OOM was contained.

It does not mean resource exhaustion can never affect unrelated services, since other shared resources may still be contested.

## 8. Finding F06 — The C++ Server Operates Under a Memory Limit

The experimental server on port 8083 was configured with:

```text
MemoryHigh=48M
MemoryMax=64M
MemorySwapMax=0
```

It started successfully and returned HTTP 200.

Observed systemd memory accounting was approximately 276 KiB at inspection time.

This confirms that the basic server does not require a large memory budget for its initial operation.

It does not prove that 64 MiB is appropriate under sustained production load.

## 9. Application-Level Memory Risks

The existing V2 worker-pool implementation includes:

```cpp
constexpr std::size_t WORKERS = 4;
constexpr std::size_t QUEUE_CAPACITY = 64;
```

These constants bound the worker count and the application queue length.

However, additional memory can be consumed by:

- Kernel socket buffers
- Accepted connection state
- Worker thread stacks
- Request and response buffers
- Logging buffers
- Future dynamically allocated request bodies
- Database-related state
- Monitoring processes

A bounded application queue does not imply that every system-level memory resource has a strict fixed bound.

### Recommended bounds

| Resource | Recommended protection |
|---|---|
| Request body | Explicit maximum size |
| Connection queue | Bounded capacity |
| Worker pool | Bounded worker count |
| Accepted connections | Admission control |
| Read/write operations | Timeouts |
| Dynamic buffers | Allocation limits |
| Service memory | systemd `MemoryMax` |
| Memory pressure | systemd `MemoryHigh`, PSI monitoring |

The systemd limit should supplement application-level protections, not replace them.

## 10. Important Limitations

The experiment did not measure how HTTP latency changes under memory pressure.

There is no evidence of a memory leak in the C++ application.

The OOM victim-selection experiment involved a simple, disposable workload rather than multiple competing application processes.

No swap experiment was conducted.

No continuous memory timeline or equivalent before/after graph has yet been collected.

The `MemoryHigh` process's manual termination means its outcome must not be labeled as successful normal completion.

## 11. Future Experiments

1. Measure HTTP request latency during `MemoryHigh` pressure.
2. Capture memory.current, memory.events, and PSI at one-second intervals.
3. Measure page faults using `/proc/vmstat` or process counters.
4. Investigate request-size limits in the HTTP parser.
5. Benchmark V2 memory consumption under increasing connection counts.
6. Evaluate systemd memory limits under sustained HTTP load.
7. Distinguish memory leaks from bounded working-set growth using repeated tests.
8. Consider memory alerts based on PSI and available memory rather than a single usage threshold.

## 12. Conclusion

The memory experiments established a progression from normal allocation to reclaim pressure and finally cgroup-local OOM.

The strongest engineering result is not that the kernel killed a process. It is that the failure occurred within an explicit resource boundary while the independent HTTP and database services survived.

A resilient application should detect and control resource growth before exhausting memory, while retaining operating-system limits as a final containment mechanism.
