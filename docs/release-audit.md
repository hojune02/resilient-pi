# v0.1.0 release audit — 2026-10-18

## Release blockers and dispositions

| Finding | Evidence in ZIP | Disposition |
|---|---|---|
| Root README empty | `README.md` zero bytes | **Fixed**: evidence-based release README |
| Git archive includes `.git` with local metadata and files unrelated to release | ZIP contains `.git/`; Git status reports untracked direct-Ethernet logs/diagnostics | **Fixed for distribution**: sanitized release ZIP excludes `.git`; original repository untouched |
| Benchmark split | `experiments/004-concurrency-comparison/comparison.md` uses campus route; Day 6 direct-Ethernet comprehensive raw files absent from this archive | README reports verifiable campus numbers and distinguishes later experiments |
| `v2` not faster | Day 6 archived CSV shows V2 throughput regression across 1–100 connections | Treat V2 as capacity/isolation experiment, not optimization |
| Low-level HTTP parser is not general HTTP server | `main.cpp`, `observable_server.cpp`: one recv; idempotent_server.cpp improved bounded header parse | Document limitations; do not claim HTTP robustness |
| Idempotency tests mutate DB | `tests/test_idempotency.py` increments `counters.requests` 3 times | Prominent warning in README and release notes |
| Current idempotent implementation is independent | `idempotent_server.cpp` on 8085, `observable_server.cpp` on 8082 | Document as experimental, not merged or supervised |
| F05 historical doc says no production HTTP idempotency | Historical `docs/incidents.md` describes state at time of experiment | README clarifies timeline; implementation added later |
| F04 configuration divergence | `f04_server.cpp` now uses TCP in both connect paths (verify) | Historical incident preserved and explanation retained |
| No tags in delivered Git snapshot implied by ZIP | Git history bundled; release tag not created by archive | Provide explicit tag/push commands; **no remote tag claimed** |
| Source/install environment | No exported deployable systemd unit, PG service configuration, Prometheus/Grafana dashboards as code | README explicitly describes external configuration prerequisites |
| Tests run on audit host | Compilation/syntax validation possible; no Pi/Postgres live environment available | Mark server runtime/integration and hardware measurements **not re-executed** |

## Unsupported claims intentionally excluded

- Production readiness or uninterrupted availability.
- V2 performance improvement (archived tests show the reverse).
- Zero data loss for arbitrary mid-COMMIT failures.
- Exactly-once network delivery or true parallel handling by single-threaded C++ server.
- Thermal throttling as cause of measured throughput regressions.
- Reliable direct-Ethernet concurrency means derived from inaccessible raw results in this ZIP.

## Pending before public tag

1. On Pi, rerun PostgreSQL idempotency tests on a **test database**.
2. Review `git diff`, `git status`, and credential scans, ensuring `.gitignore` excludes build artifacts and credentials.
3. Record actual `systemctl cat resilient-pi` and reproducible Linux stack provisioning only if claiming turnkey deployment.
4. Decide if diagnostic source files should be committed (keep distinct from baseline V2).
5. Confirm figures' originating dataset and remove untraceable visualizations.
6. Tag the reviewed commit `v0.1.0` and push the tag after tests pass.
