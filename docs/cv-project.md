# CV / interview wording — Resilient Pi

**Recommended project name:** Resilient Pi — C++/Linux Failure-Injection Testbed

**One-line:** Built and benchmarked a Raspberry Pi-based C++ HTTP reliability testbed with systemd crash recovery, PostgreSQL, bounded worker-pool experiments, Prometheus metrics, and controlled failure injection.

**CV bullets (use only if you personally verify the code, experiments and measurements):**

- Built POSIX-socket C++ HTTP servers and compared a sequential baseline with a four-worker, 64-entry bounded-queue design; documented throughput regression and overload/rejection behavior instead of claiming unverified speedups.
- Measured process failure recovery with timed HTTP probes (20 archived trials: median ~2.17 s), and reproduced isolated memory-cgroup OOM while unrelated HTTP/PostgreSQL services remained available.
- Implemented PostgreSQL-backed counter operations, readiness/liveness monitoring, and an experimental transactional idempotency-key endpoint; injected dependency failures and examined ambiguous-commit retry risks.

**Do not claim:** production-grade server, proven high concurrency scale, exactly-once network delivery, proven zero data loss, or V2 performance improvement.

**Two-minute interview story:** HTTP readiness passed, but the expected database TCP socket didn't appear in `ss`; investigated process file descriptors, located inconsistent libpq connection strings, unified startup/reconnect configuration, then verified TCP connectivity and targeted PostgreSQL backend recovery.
