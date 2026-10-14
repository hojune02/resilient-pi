
# Day 10 — Memory Pressure, OOM, and Resource Isolation

**Date:** 2026-10-15  
**Project:** Resilient Pi  
**Topic:** Linux memory management, cgroups, OOM, and bounded resources

## 1. Objective

Investigate how a Linux system behaves as memory demand approaches or exceeds configured resource limits.

The goal was to understand memory accounting, failure behavior, and isolation rather than exhaust the Raspberry Pi's physical memory.

The experiment also aimed to identify memory-related protections that could improve the C++ server's reliability.

## 2. Starting Point

Previous project milestones established:

- A C++ HTTP server managed by systemd
- A bounded worker-pool implementation
- PostgreSQL persistence and database failure recovery
- Prometheus and Grafana observability
- CPU saturation and scheduler-contention experiments

Day 10 extends the reliability investigation from CPU scheduling to memory resource management.

## 3. Inspecting Linux Memory

I inspected the Raspberry Pi using:

```bash
free -h
swapon --show
cat /proc/meminfo
cat /proc/pressure/memory
vmstat 1 5
```

The system reported:

```text
Total memory: 7.6 GiB
Used memory: approximately 2.0 GiB
Available memory: approximately 5.6 GiB
Swap: 0 B
```

Memory PSI:

```text
some avg10=0.00
full avg10=0.00
```

The machine was not experiencing measurable recent memory pressure.

### Learning

Memory usage alone is not sufficient to diagnose memory scarcity.

Linux can use a substantial amount of memory for reclaimable caches while retaining significant memory availability.

## 4. Inspecting Cgroup Support

The Raspberry Pi used cgroup v2.

```text
cgroup2fs
```

The memory controller was available.

The existing C++ service had no configured finite memory hard limit:

```text
MemoryMax=infinity
```

I decided to use disposable systemd transient units for testing rather than changing the production HTTP service.

### Learning

Cgroups allow Linux to account for and limit resource usage by a group of processes.

This provides a mechanism for isolating resource-intensive workloads.

## 5. Initial Memory Allocation Probe

I created:

```text
experiments/008-memory-pressure/allocate.py
```

The program allocated a 192 MiB byte array and touched its pages.

The process reported:

```text
VmSize: 211332 kB
VmRSS:  206160 kB
VmHWM:  206160 kB
```

Live cgroup memory usage was approximately 196 MiB.

The program completed normally, and no memory-high or OOM events were recorded.

### Unexpected observation

The first transient service invocation reported only 652 KiB peak memory despite the Python program showing approximately 201 MiB RSS.

A second invocation and live cgroup readings confirmed memory usage consistent with the requested allocation.

The discrepancy was not fully explained.

### Learning

Memory measurements should be cross-checked using multiple sources.

Process RSS, virtual address space, and cgroup memory accounting are not interchangeable.

## 6. Testing MemoryHigh

I requested 300 MiB while configuring:

```text
MemoryHigh=256M
MemoryMax=384M
MemorySwapMax=0
```

The kernel reported:

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

The allocation did not complete normally during the expected interval.

It remained running until I manually stopped the service several minutes later.

### Learning

`MemoryHigh` does not act as a hard allocation limit.

It can cause reclaim, throttling, and severe execution delays without killing a process.

This matters because a service can remain alive but stop making acceptable progress.

## 7. Triggering a Cgroup-Local OOM

I requested a 512 MiB allocation with:

```text
MemoryMax=384M
MemorySwapMax=0
OOMPolicy=stop
```

The kernel reported:

```text
oom-kill:constraint=CONSTRAINT_MEMCG
task=python3
pid=35018
```

Systemd reported:

```text
Result=oom-kill
ExecMainStatus=9
```

The Python process was killed using SIGKILL.

### Learning

The OOM event was caused by the experimental cgroup's memory limit rather than exhaustion of the Pi's total physical memory.

The hard limit converted uncontrolled growth into a failure contained within the designated resource group.

## 8. Verifying Failure Isolation

After the OOM, I checked both application services.

Observed:

```text
resilient-pi: active
postgresql@16-main: active
```

The HTTP server returned HTTP 200.

The host retained more than 5 GiB of available memory.

### Learning

The memory-exhaustion failure was isolated from the C++ HTTP server and PostgreSQL in the observed experiment.

Independent resource boundaries can reduce the impact of failures across processes.

## 9. Running a Memory-Limited HTTP Server

I copied the original V1 implementation to:

```text
experiments/008-memory-pressure/limited_server.cpp
```

The experimental copy listened on port 8083.

It ran under:

```text
MemoryHigh=48M
MemoryMax=64M
MemorySwapMax=0
```

The server responded with HTTP 200.

After stopping the limited instance, the original C++ service and PostgreSQL remained active.

### Learning

A service can operate with an explicit memory budget.

However, one successful HTTP request does not prove that the configured limit is sufficient under high concurrency or sustained load.

## 10. Engineering Questions

### Q1. What is the difference between virtual memory and resident memory?

Virtual memory describes the address space a process has mapped or reserved.

Resident memory describes pages currently held in physical RAM.

A process may reserve virtual address space without making every page resident.

In the allocation probe, writing to each page produced a measurable resident working set.

The probe reported approximately 211 MB of virtual memory and 206 MB of RSS in Linux's reported kB units.

### Q2. Why can low free memory be normal on Linux?

Linux uses otherwise unused memory for page cache and other performance-related purposes.

This memory can often be reclaimed when applications require it.

The baseline showed approximately 2 GiB free but approximately 5.6 GiB available.

Low `MemFree` alone does not prove the system is under memory pressure.

### Q3. What is the page cache?

The page cache stores file-backed data in RAM to reduce repeated storage access.

It can improve performance by avoiding slower disk or microSD reads.

Under memory pressure, the kernel can reclaim eligible cached pages.

Therefore, cache usage should not automatically be classified as wasted memory.

### Q4. When does swap help, and when does it only delay failure?

Swap allows some anonymous memory pages to be moved out of RAM.

It may help workloads with infrequently accessed memory.

However, heavy swapping can create significant latency, especially on slower storage.

If a workload continually accesses more memory than physical RAM can accommodate, swapping may cause severe performance degradation rather than solve the underlying capacity problem.

This project had no swap configured, so these behaviors were not tested directly.

### Q5. Which process does Linux kill under OOM, and why?

The kernel's decision depends on the OOM constraint and eligible processes.

For a memory-cgroup OOM, the victim is selected from processes subject to the relevant memory constraint.

In this experiment, the kernel killed Python PID 35018 inside:

```text
/system.slice/resilient-pi-memory-oom.service
```

The kernel reported `CONSTRAINT_MEMCG`, identifying the cgroup memory limit as the trigger.

The test did not investigate victim selection among multiple eligible processes.

### Q6. How can you distinguish a memory leak from legitimate memory growth?

A memory leak occurs when memory remains allocated without a continuing useful purpose.

Legitimate growth occurs when a process retains memory needed for active work, caching, or bounded state.

To investigate, I would:

1. Measure memory under a controlled workload.
2. Stop the workload.
3. Allow the application to return to idle.
4. Observe whether memory stabilizes or continues growing.
5. Inspect allocations, object lifetimes, and retained resources.

A stable RSS alone does not prove there is no leak, and high RSS alone does not prove there is one.

No memory leak was established in this project.

### Q7. What resource in your server grows with client count?

Relevant resources include:

- Accepted socket descriptors
- Kernel socket state and buffers
- Pending connection queue entries
- Worker stacks
- Per-request buffers and state

The V2 server has four workers and a 64-entry application queue.

These bounds limit some resource growth, but not all kernel- and application-level memory usage.

### Q8. Where should you impose an upper bound?

Bounds should be imposed at several layers:

- Maximum request-header and body sizes
- Maximum active requests
- Maximum pending connection queue length
- Worker count
- Read and write timeouts
- Dynamic buffer allocation
- Service memory usage through systemd or cgroups

Application bounds should prevent uncontrolled growth before the OS reaches an OOM condition.

### Q9. What happens to latency before OOM occurs?

Memory pressure can introduce allocation stalls, reclaim work, page faults, and potentially swapping delays.

In the `MemoryHigh` experiment, memory PSI showed substantial stalls and the allocation failed to complete promptly.

This supports the conclusion that memory pressure can delay progress without immediately causing OOM.

However, HTTP latency was not measured concurrently with this pressure experiment.

Therefore, the experiment cannot quantify HTTP latency degradation before OOM.

### Q10. Can resource limits make failure more predictable?

Yes, within the tested constraints.

The 384 MiB hard limit caused the experimental allocation process to be killed inside its cgroup.

The C++ HTTP server and PostgreSQL survived.

The 64 MiB limited-server experiment also showed that the HTTP server could function with a finite memory budget.

Resource limits can improve failure containment, but they do not eliminate all risks from shared CPU, I/O, and memory pressure.

## 11. Design Decisions

The main engineering decision is to combine:

1. Application-level bounds on resource growth
2. systemd memory limits for service isolation
3. Monitoring of memory availability and pressure
4. Explicit overload behavior before memory exhaustion

I would not rely on OOM termination as the normal method of handling excess workload.

For the production server, memory limits should be selected after measuring realistic sustained workloads and retaining sufficient operational headroom.

## 12. What Was Not Proven

- A memory leak in the C++ server
- The correct production memory limit for V1 or V2
- HTTP latency degradation during memory pressure
- The behavior of a swap-enabled system
- OOM victim selection among multiple processes
- Continuous memory growth under sustained concurrency
- A complete before/after memory-pressure timeline

These remain possible future experiments rather than conclusions from Day 10.

## 13. Personal Engineering Reflection

**What surprised me about `MemoryHigh`:**  
TODO — describe why the process remained alive but failed to make normal progress.

**What I learned about process isolation:**  
TODO — explain why PostgreSQL and the HTTP service survived the OOM.

**Which memory metric I initially misunderstood:**  
TODO — compare `MemFree`, `MemAvailable`, RSS, or cgroup memory.

**What I would change before production deployment:**  
TODO — describe application bounds, realistic workload measurements, and resource headroom.

**Which experiment I would repeat more carefully:**  
TODO — consider a time-aligned memory-pressure and HTTP-latency test.

## 14. Day 10 Outcome

The experiment demonstrated normal memory allocation, memory-high enforcement, cgroup-local OOM, and independent service survival.

A disposable C++ server also ran successfully with a 64 MiB hard limit.

The results support a layered approach to resource protection:

**Bound application memory growth first, then use operating-system memory limits to contain unexpected failures.**

## 15. Next Steps

- Preserve the final experiment evidence in Git.
- Build a before/after memory graph using clearly labeled actual observations.
- Measure HTTP latency under memory pressure.
- Investigate request-size and per-client memory bounds in the C++ server.
- Evaluate production service memory limits using sustained workload measurements.
- Continue with Day 11 of the project roadmap.
