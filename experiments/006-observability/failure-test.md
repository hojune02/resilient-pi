# Day 8 — PostgreSQL Failure Observability Test

## Objective

Verify that application metrics distinguish HTTP process liveness from PostgreSQL dependency readiness and preserve evidence of an outage after recovery.

## Fault injection

Stopped PostgreSQL using `sudo systemctl stop postgresql@16-main`, while leaving the observable C++ server running.

## Observations

| Measurement | During outage | After recovery |
|---|---:|---:|
| `GET /health` | HTTP 200 | Not retested |
| `GET /ready` | HTTP 503 | HTTP 200 |
| `GET /counter` | HTTP 503 | HTTP 200, value 16 |
| HTTP request counter | 8 | 11 |
| HTTP error counter | 2 | 2 |
| Database connection gauge | 0 | 1 |
| Database operation histogram count | 2 | 3 |

## Interpretation

The HTTP process remained responsive during the database outage, but database-dependent operations were unavailable.

The readiness check exposed the dependency failure, while the liveness check continued reporting that the HTTP application was running.

The database connection gauge changed from 0 to 1 after recovery. The HTTP error counter retained the two observed failures.

## Limitations

- The database connection gauge represents the last measured state rather than continuous availability.
- A successful metrics scrape does not guarantee every application operation is healthy.
- Metrics collection uses the same single-threaded HTTP execution loop as application requests.
- The experiment did not measure detection latency or Prometheus scrape delay.
- Request metrics are local to the current process lifetime and reset on restart.

## Evidence

- `results/raw/day08/metrics-before-failure.txt`
- `results/raw/day08/metrics-during-db-failure.txt`
- `results/raw/day08/metrics-after-recovery.txt`
