# Experiment 007 — CPU Saturation Findings

**Date:** 2026-10-14  
**Hardware:** Raspberry Pi 4B, four CPU cores, approximately 8 GB RAM  
**Application:** Resilient Pi V1 C++ HTTP server

## Objective

Investigate how external CPU pressure affects HTTP throughput, latency, scheduler contention, temperature, and clock frequency.

## Experimental setup

- Load generator: Arch Linux via direct Ethernet
- Server: Raspberry Pi at `10.42.0.2:8080`
- Workload: `wrk -t2 -c50 -d15s --latency --timeout 5s`
- CPU pressure: `stress-ng`, 0–4 CPU workers
- Monitoring: `mpstat`, `vmstat`, thermal sysfs and CPU frequency sysfs

## Observations

| Stress workers | Throughput (req/s) | p50 (ms) | p99 (ms) |
|---|---:|---:|---:|
| 0, initial | 2753.95 | 5.48 | 371.64 |
| 1 | 2566.94 | 6.78 | 446.39 |
| 2 | 2201.67 | 10.78 | 269.48 |
| 3, repeat | 1929.54 | 12.31 | 273.89 |
| 4, verification | 2092.75 | 11.02 | 223.32 |
| 0, recovery | 2841.86 | 5.09 | 421.96 |

The CPU4 verification ran from approximately 23:28:29 to 23:28:44 KST.

During that interval:

- Mean host CPU utilization: 99.9%
- Peak one-second CPU utilization: 100%
- Mean temperature: 55.12°C
- Temperature range: 52.58–57.45°C
- `vmstat` showed runnable-task counts frequently above four

The CPU4 experiment produced approximately 24% lower throughput than the initial baseline.

## Thermal observations

The highest sampled temperature across the initial CPU4 pressure experiment was approximately 62.81°C.

CPU0 samples remained at 1.8 GHz during the stressed monitoring windows.

`vcgencmd get_throttled` returned `0x0`.

No thermal-throttling evidence was observed in the available measurements.

The effect of the cooling fan was not independently tested.

## Interpretation

External CPU pressure increased scheduler contention.

With four cores already heavily utilized, application and kernel threads competed for CPU execution time.

Under the tested workload, this coincided with reduced throughput and higher median latency.

The evidence is consistent with scheduler contention contributing to performance degradation, rather than thermal throttling.

## Limitations

- Primarily one trial per pressure level
- Benchmark alignment was verified most clearly for the CPU4 repeat
- CPU3 experiments were repeated, and some earlier output was overwritten
- Measurements include client and system-level variability
- No controlled fan-on/fan-off comparison
- CPU frequency sampling focused on CPU0
- No dedicated per-process scheduler latency measurements
- No production workload or long-duration soak test
- p99 results are non-monotonic and should not be interpreted as a stable trend

## Conclusion

CPU saturation can reduce application throughput and increase median request latency even when the processor maintains its configured operating frequency.

System-level CPU utilization, runnable-task pressure, and request latency should be analyzed together.

## Future work

- Repeat randomized pressure-level trials
- Measure scheduling delays and process CPU allocation
- Test CPU limits and priorities through systemd or cgroups
- Evaluate application admission control under overload
- Compare cooling configurations under sustained load
