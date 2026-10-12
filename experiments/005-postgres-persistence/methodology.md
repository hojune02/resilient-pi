
# Experiment 005 — PostgreSQL Persistence and Failure Recovery

## 1. Objective

Investigate persistence, transaction correctness, database connection lifecycle, and failure recovery in a C++ HTTP server backed by PostgreSQL.

The experiment aims to answer:

1. Does committed state survive application and database restarts?
2. How does the application behave when PostgreSQL becomes unavailable?
3. Can the application recover without restarting its own process?
4. Can PostgreSQL handle concurrent updates without losing increments?
5. What durability guarantees does PostgreSQL provide?
6. What resources are associated with database connections?

The goal is to understand systems behavior rather than implement a complex application.

## 2. Environment

| Component | Configuration |
|---|---|
| Hardware | Raspberry Pi 4 Model B, 4 CPU cores, approximately 8 GB RAM |
| Operating system | Ubuntu Server 24.04 ARM64 |
| Database | PostgreSQL 16.15 |
| Database cluster | `16/main` |
| Database name | `resilient_pi` |
| Application role | `hojune` |
| Database client library | `libpq` |
| Application | Single-threaded C++20 HTTP server |
| HTTP bind address | `127.0.0.1:8081` |
| Database transport | Unix-domain socket |
| Authentication | PostgreSQL peer authentication |
| Storage | microSD-backed filesystem |
| Build optimization | `-O2` |

The original V1 and V2 servers were preserved. Day 7 introduced a separate implementation in `server/persistent_server.cpp`.

## 3. Architecture

```text
HTTP client
    |
    | TCP, loopback interface
    v
C++ persistent_server
    |
    | libpq, Unix-domain socket
    v
PostgreSQL backend process
    |
    | SQL transaction
    v
PostgreSQL WAL and persistent storage
    |
    v
HTTP response
```

The HTTP application and PostgreSQL are independent processes with distinct failure lifecycles.

The application maintains one PostgreSQL connection across multiple HTTP requests.

## 4. Database Schema

Source: `database/schema.sql`

```sql
CREATE TABLE IF NOT EXISTS counters (
    name TEXT PRIMARY KEY,
    value BIGINT NOT NULL DEFAULT 0
);

INSERT INTO counters (name, value)
VALUES ('requests', 0)
ON CONFLICT (name) DO NOTHING;
```

The `requests` counter was selected to minimize domain complexity while allowing transaction and persistence testing.

## 5. HTTP Interface

| Method | Endpoint | Behavior |
|---|---|---|
| GET | `/health` | Application liveness |
| GET | `/counter` | Read persistent counter |
| POST | `/counter/increment` | Increment counter transactionally |

Database-dependent endpoints return HTTP 503 when database operations fail.

The application binds to localhost only. It is not publicly exposed.

## 6. Transaction Implementation

A successful increment follows:

```text
BEGIN
  |
  v
UPDATE counters
SET value = value + 1
WHERE name = 'requests'
RETURNING value
  |
  v
COMMIT
  |
  v
HTTP 200 + committed counter value
```

The application uses `PQexec()` to execute SQL statements.

`PGconn` and `PGresult` resources are managed with RAII wrappers using `std::unique_ptr` and custom deleters.

One database connection is reused across HTTP requests.

### Transaction correctness limitations

If a database connection is lost while a COMMIT operation is in progress, the client may not know whether PostgreSQL committed the transaction.

Blindly retrying a non-idempotent increment can therefore cause duplicate updates.

The implementation does not automatically retry failed transactions after execution begins.

## 7. Experiments

### P01 — Normal Database Operation

**Procedure**

1. Read the initial counter through HTTP.
2. Increment the counter through HTTP.
3. Read the updated value.

**Observed**

```text
GET /health             -> HTTP 200, OK
GET /counter            -> HTTP 200, 3
POST /counter/increment -> HTTP 200, 4
GET /counter            -> HTTP 200, 4
```

**Conclusion**

The application successfully performs the complete HTTP → C++ → PostgreSQL → HTTP request path.

### P02 — Application Process Restart

**Hypothesis**

Committed counter state will survive termination and restart of the C++ server.

**Procedure**

1. Verify counter value 4.
2. Restart the C++ application.
3. Read the counter.
4. Increment once more.

**Observed**

```text
Before restart: 4
After restart:  4
After increment: 5
```

**Conclusion**

The counter is owned by PostgreSQL, not by the C++ application's process memory.

### P03 — PostgreSQL Restart Persistence

**Hypothesis**

Committed database state survives an orderly PostgreSQL service restart.

**Procedure**

Restart the PostgreSQL cluster and query the previously committed counter state.

**Observed**

PostgreSQL returned the committed counter value 5 following restart.

**Conclusion**

Committed state survived the observed database restart.

**Limitation**

An orderly restart is not equivalent to sudden power loss or physical media failure.

### P04 — Transaction Rollback

**Procedure**

Execute an increment inside a transaction and issue ROLLBACK instead of COMMIT.

**Observed**

The counter remained at its previously committed value following rollback.

**Conclusion**

Changes made within an aborted transaction were not retained as committed state.

### P05 — Database Outage Before Connection Recovery Fix

**Procedure**

1. Start the C++ server with PostgreSQL available.
2. Stop the PostgreSQL cluster.
3. Request `/counter` and `/health`.
4. Restart PostgreSQL.
5. Repeat `/counter`.

**Observed**

```text
PostgreSQL available:
GET /counter -> HTTP 200

PostgreSQL stopped:
GET /counter -> HTTP 503
GET /health  -> HTTP 200

PostgreSQL restarted:
Direct SQL query -> succeeded, value 5
GET /counter     -> HTTP 503 repeatedly
```

**Finding**

PostgreSQL recovered, but the C++ application continued using an invalid `PGconn`.

The application remained alive while its database-dependent functionality remained unavailable.

### P06 — Database Connection Recovery Fix

**Hypothesis**

Replacing an invalid PostgreSQL connection before a new database operation will restore functionality without restarting the application.

**Implementation**

Added `ensure_connected()` to inspect the connection state using `PQstatus()` and create a replacement connection when necessary.

**Observed**

```text
Before outage:
GET /counter -> HTTP 200, value 5

During outage:
GET /counter -> HTTP 503
GET /health  -> HTTP 200

After PostgreSQL restart:
GET /counter -> HTTP 200, value 5

Subsequent write:
POST /counter/increment -> HTTP 200, value 6
```

**Conclusion**

The observed persistent connection failure was resolved for requests made after PostgreSQL became available again.

**Limitations**

- Reconnection attempts currently have no application-level backoff.
- Recovery time was not quantitatively measured.
- Connection recovery is different from safe transaction retry.
- An interrupted COMMIT remains a potentially ambiguous outcome.

### P07 — Concurrent Atomic Updates

**Hypothesis**

PostgreSQL will coordinate concurrent updates to the same row without losing increments.

**Procedure**

Execute ten independent `psql` processes concurrently:

```bash
seq 1 10 | xargs -P 10 -I {} \
  psql -X -q -v ON_ERROR_STOP=1 -d resilient_pi -c \
  "BEGIN;
   UPDATE counters
   SET value = value + 1
   WHERE name = 'requests';
   COMMIT;"
```

**Observed**

```text
Initial value: 6
Parallel increments: 10
Final value: 16
```

No SQL errors were reported.

**Conclusion**

The atomic SQL update correctly preserved all ten increments in the observed test.

This validates concurrent database updates, not concurrent HTTP processing. The Day 7 HTTP server remains single-threaded.

## 8. Database Connection Lifecycle

Inspection of `pg_stat_activity` showed:

| Connection | State |
|---|---|
| Persistent C++ application connection | Idle |
| Diagnostic `psql` connection | Active |

A PostgreSQL backend process was associated with the application's persistent connection.

The observed backend RSS was approximately 16.5 MiB.

PostgreSQL reported:

```text
max_connections = 100
superuser_reserved_connections = 3
```

The RSS of PostgreSQL backend processes cannot simply be summed to determine incremental memory usage because shared memory pages may appear in multiple processes.

### Connection management decision

A single reused connection is sufficient for the current single-threaded HTTP implementation.

A future multithreaded HTTP server should use an explicitly bounded database connection pool or another carefully synchronized connection-management strategy.

Opening a new database connection for every HTTP request was not implemented or benchmarked.

## 9. WAL and Durability

Observed PostgreSQL settings:

| Setting | Value |
|---|---|
| `fsync` | `on` |
| `synchronous_commit` | `on` |
| `full_page_writes` | `on` |
| `wal_level` | `replica` |
| `wal_sync_method` | `fdatasync` |

Observed WAL LSN:

```text
0/19863D8
```

With the current local durability settings, PostgreSQL normally flushes the necessary WAL commit information before acknowledging a synchronous COMMIT.

Modified table pages may be written to their final locations later.

Durability still depends on the underlying storage correctly honoring synchronization requests.

The recorded WAL LSN is an observation of log position; it does not independently prove the durability of any particular transaction.

## 10. Key Findings

1. The database and application have independent process lifecycles.
2. Committed state survives application and orderly database restarts.
3. PostgreSQL transactions can preserve atomic increments under concurrent updates.
4. A running HTTP application may remain alive while its database dependency is unavailable.
5. PostgreSQL recovery does not automatically restore a broken application connection.
6. Explicit connection recovery resolves the observed stale-connection failure.
7. Connection reuse avoids repeated connection establishment.
8. Reconnection and transaction retry have different correctness requirements.
9. WAL provides a mechanism for crash recovery and durable commit acknowledgment.
10. The database-backed implementation remains a learning prototype, not a production-ready HTTP service.

## 11. Outstanding Work

- Measure database-query latency and connection-recovery latency.
- Add bounded reconnection backoff.
- Investigate ambiguous COMMIT outcomes and idempotency.
- Benchmark connection reuse against per-request connections.
- Integrate a bounded connection pool if the HTTP architecture becomes concurrent.
- Test application readiness separately from process liveness.
- Instrument database availability, request latency and error rates on Day 8.
- Evaluate storage durability under controlled failure conditions without risking filesystem corruption.

## 12. Evidence

Related files:

- `database/schema.sql`
- `server/persistent_server.cpp`
- `experiments/005-postgres-persistence/db_smoke.cpp`
- `results/raw/day07/db-recovery-before-fix.md`
- `results/raw/day07/db-recovery-after-fix.md`
- `docs/design-decisions.md`

Some test outputs were recorded in terminal sessions and may not yet exist as separate raw files in the repository. Those files should only be referenced as available evidence after they have actually been saved.
