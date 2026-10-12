# P05 — PostgreSQL Dependency Recovery (Before Fix)

## Initial state
- Counter value: 5
- C++ application: running on 127.0.0.1:8081
- PostgreSQL 16 cluster: online

## Fault injection
Stopped PostgreSQL with:
`sudo systemctl stop postgresql@16-main`

## Observations
- GET /counter → HTTP 503
- GET /health → HTTP 200
- Restarted PostgreSQL.
- Direct SQL SELECT → value 5
- GET /counter → HTTP 503 on three repeated attempts

## Finding
The application remained alive but its persistent PostgreSQL connection became unusable. Restarting PostgreSQL alone did not restore database-backed HTTP functionality.

## Hypothesis
The C++ application retains a failed PGconn and never calls PQreset() or creates a replacement PGconn.

## Next experiment
Introduce connection recovery and repeat the fault injection.
