# Resilient Pi v0.1.0 — Release Candidate

**Status:** Candidate assembled 2026-10-18; **tag not created and Pi integration not rerun in this workspace**.

This release preserves measured experiments rather than presenting the system as optimized or production-ready. See [README.md](README.md) and [docs/release-audit.md](docs/release-audit.md).

- V1 supervised POSIX-socket HTTP baseline, separate V2 bounded worker-queue prototype.
- PostgreSQL persistence/reconnection, health/readiness distinction, local metrics/observability prototype.
- CPU saturation, cgroup memory pressure/OOM, and five multi-failure incident investigations with raw evidence.
- Transactional request-key idempotency server as a separate experimental prototype on port 8085.

Known limitations: benchmark route/confounders; incomplete direct-Ethernet archived sweeps; PostgreSQL integration pending on target Pi; independent server variants not unified; idempotency test changes database state. Do not claim true end-to-end exactly-once delivery.
