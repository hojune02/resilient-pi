
# Day 7 — PostgreSQL Persistence and Database Failure Recovery

**Date:** 2026-10-13  
**Project:** Resilient Pi  
**Topic:** Transactions, persistence, connection lifecycle and dependency recovery

## 1. Objective

Extend the existing C++ server with persistent state while investigating how an application interacts with an independently running database process.

The goal was not to build a feature-rich application, but to understand:

- SQL transactions and commit behavior
- PostgreSQL WAL and persistence
- Database connection lifecycle
- Failure boundaries between processes
- Recovery after database unavailability
- Correctness of concurrent updates

## 2. Initial State

The project already contained:

- V1: single-threaded blocking TCP/HTTP server
- V2: bounded worker-pool server
- systemd process supervision
- crash-recovery experiments
- performance and concurrency benchmarks

Neither server stored durable application state.

For Day 7, I introduced a separate PostgreSQL-backed implementation while preserving the previous server architectures.

## 3. Environment Setup

PostgreSQL 16.15 was installed on the Raspberry Pi.

The machine had:

- Four CPU cores
- Approximately 8 GB RAM
- Approximately 51 GB available storage at the initial environment check
- Ubuntu Server 24.04 ARM64

The database cluster was `16/main`.

PostgreSQL listened on loopback addresses rather than the campus-facing interface.

I created:

```text
Database: resilient_pi
Role: hojune
Table: counters
```

The application uses a local Unix-domain socket with peer authentication.

This avoids storing a database password in the source code.

## 4. First SQL Transactions

I created a single-row counter and tested `BEGIN`, `UPDATE`, `COMMIT`, and `ROLLBACK`.

The counter correctly retained a committed increment, while a rolled-back increment did not change the committed value.

This established the difference between modifying data within a transaction and making that modification committed state.

## 5. Connecting C++ to PostgreSQL

I wrote a standalone C++ program using `libpq`.

The program:

1. Established a PostgreSQL connection.
2. Began a transaction.
3. Incremented the counter.
4. Read the updated value using `RETURNING`.
5. Committed the transaction.
6. Released its PostgreSQL resources.

The first run incremented the counter from 1 to 2.

The second run incremented it from 2 to 3.

I also encountered a database-name error while attempting to connect to `resilient-pi` rather than `resilient_pi`.

The correct database name uses an underscore.

### Learning

The PostgreSQL connection is an independently managed resource.

A connection can execute multiple transactions, and its lifecycle does not need to match an individual SQL statement or HTTP request.

## 6. Implementing the HTTP Database Operation

I created `server/persistent_server.cpp`, listening on:

```text
127.0.0.1:8081
```

The API contains:

| Endpoint | Purpose |
|---|---|
| `GET /health` | Application liveness |
| `GET /counter` | Read persistent state |
| `POST /counter/increment` | Commit a persistent increment |

Initial observations:

```text
GET /health             -> HTTP 200, OK
GET /counter            -> HTTP 200, 3
POST /counter/increment -> HTTP 200, 4
GET /counter            -> HTTP 200, 4
```

The application was able to transform an HTTP request into an SQL operation and return the resulting state.

The HTTP parser remains deliberately limited and does not implement complete HTTP framing.

## 7. Application and Database Restart Experiments

After restarting the C++ application, the counter retained value 4.

A subsequent increment changed the value to 5.

PostgreSQL also retained the committed value through the observed orderly database restart.

### Learning

Persistent state belongs to the database, not the lifetime of the HTTP process.

Restarting the application should not recreate or reset durable state.

## 8. Failure Injection — The First Recovery Problem

I deliberately stopped PostgreSQL while the C++ server remained running.

Observed:

```text
GET /counter -> HTTP 503
GET /health  -> HTTP 200
```

The HTTP process remained alive, but database-dependent functionality failed.

After restarting PostgreSQL, I verified that the database was online and its counter still held value 5.

However:

```text
GET /counter -> HTTP 503
GET /counter -> HTTP 503
GET /counter -> HTTP 503
```

The application did not automatically recover its connection.

### Root-cause hypothesis

The application continued holding a `PGconn` that had become invalid when PostgreSQL stopped.

Restarting the database process did not repair the existing connection object.

### Why this mattered

The process supervisor could consider the HTTP application healthy even though an essential operation was unavailable.

This demonstrated why application liveness is insufficient as a measure of service readiness.

## 9. Implementing Connection Recovery

I added `ensure_connected()` to detect an invalid PostgreSQL connection and replace it before executing a new database operation.

I deliberately kept the recovery mechanism small:

- One persistent database connection
- No general-purpose pool
- No automatic retry of potentially ambiguous increments
- HTTP 503 while the dependency remains unavailable

After rebuilding, I repeated the failure experiment.

Observed:

```text
Before failure:
GET /counter -> HTTP 200, 5

PostgreSQL stopped:
GET /counter -> HTTP 503
GET /health  -> HTTP 200

PostgreSQL restarted:
GET /counter -> HTTP 200, 5

Subsequent increment:
POST /counter/increment -> HTTP 200, 6
```

The application recovered connectivity without restarting its own process.

### Remaining uncertainty

If the connection fails while a COMMIT is in progress, the client may not know whether the database committed the transaction.

Reconnecting alone does not solve that ambiguity.

## 10. Concurrent Transaction Test

I executed ten independent PostgreSQL transactions in parallel using `psql` processes.

Observed:

```text
Initial value: 6
Concurrent increments: 10
Final value: 16
```

No SQL errors were reported.

PostgreSQL correctly coordinated conflicting updates to the same row in the observed test.

This is a database concurrency test, not proof that the single-threaded HTTP server executes requests concurrently.

## 11. Connection and Resource Inspection

Using `pg_stat_activity`, I observed:

- One idle connection retained by the C++ server
- One active diagnostic connection used by `psql`

PostgreSQL maintains a backend process for each connection.

The application connection's backend had an observed RSS of approximately 16.5 MiB, including potentially shared memory.

The configured maximum was 100 connections, including three reserved for superusers.

I concluded that a single reused connection is appropriate for the current single-threaded application.

A bounded connection pool becomes more relevant when multiple HTTP workers can execute database queries concurrently.

## 12. WAL Investigation

Observed settings:

```text
fsync = on
synchronous_commit = on
full_page_writes = on
wal_level = replica
wal_sync_method = fdatasync
```

PostgreSQL uses write-ahead logging to support crash recovery.

With the observed settings, it normally synchronizes the required WAL information before acknowledging a successful synchronous COMMIT.

Modified data pages do not necessarily need to be written to their final table locations before acknowledgment.

This guarantee assumes the storage device honors synchronization requests.

## 13. Engineering Questions

### Q1. What does it mean for a transaction to be committed?

A transaction has successfully completed the database's commit protocol, and its changes become committed state.

For this project, a successful PostgreSQL COMMIT means the database accepted the transaction and, under the observed durability configuration, normally flushed the required WAL information.

A committed transaction differs from an uncommitted or rolled-back transaction.

### Q2. When PostgreSQL acknowledges COMMIT, what must be true on durable storage?

With `fsync=on` and `synchronous_commit=on`, the required WAL commit records must normally have been flushed to the storage system before acknowledgment.

The corresponding modified table pages do not all need to be written immediately.

This relies on the storage stack honoring synchronization requests.

### Q3. What is WAL, and why write it before data pages?

WAL is PostgreSQL's write-ahead log.

It records changes needed for recovery.

By ensuring relevant log records reach durable storage before dirty data pages, PostgreSQL can reconstruct committed database state after a crash.

### Q4. Why can database latency dominate application latency?

A database operation can involve IPC, query execution, locks, storage I/O, WAL synchronization and scheduling.

Even if the HTTP handler performs very little CPU work, it may spend significant wall-clock time waiting for PostgreSQL.

Our experiment did not quantify the individual latency contributions.

### Q5. What happens if a new database connection is opened for every request?

Each request incurs connection establishment and authentication costs.

PostgreSQL also allocates backend process resources per connection.

At high request rates, connection churn can consume CPU and memory, while connection limits may become a bottleneck.

We have not benchmarked this alternative.

### Q6. What resource does a connection pool protect?

A bounded connection pool limits the number of concurrently established database sessions and therefore helps control backend processes, memory use, and database-side concurrency.

It also allows applications to reuse connections rather than repeatedly creating them.

### Q7. How many DB connections should a four-core Pi maintain?

There is no universal number derived from core count alone.

Our current single-threaded HTTP server needs only one reused connection.

For a future four-worker implementation, two to four connections would be a starting hypothesis rather than a proven optimum.

The correct configuration requires workload measurements.

### Q8. What happens when all connections are busy?

New database operations must wait for a connection, fail after a timeout, or be rejected according to the application's admission policy.

An unbounded waiting queue can cause increasing latency and memory pressure.

A bounded pool should define a clear overload policy.

### Q9. How does database failure differ from application failure?

They affect different processes and can occur independently.

In our experiment, PostgreSQL was stopped while the HTTP server remained alive.

The server still answered `/health` but returned HTTP 503 for database-dependent operations.

### Q10. What state should the application preserve if PostgreSQL becomes temporarily unavailable?

It should preserve its own operational state without inventing successful database writes.

For transactions with unknown commit outcomes, it must preserve uncertainty rather than blindly retrying non-idempotent operations.

A production implementation may require durable request identifiers, idempotency keys, retry state or a transactional outbox depending on requirements.

Our implementation does not yet provide those mechanisms.

## 14. Personal Engineering Reflection

**What surprised me most:** TODO — write in my own words.

**Which hypothesis did I make before observing the database failure?** TODO.

**What evidence changed my understanding?** TODO.

**What would I design differently if the application had 100 simultaneous clients?** TODO.

**Which part of the implementation can I explain confidently without AI assistance?** TODO.

## 15. Remaining Limitations

- HTTP handling remains single-threaded.
- HTTP request parsing is incomplete.
- Database queries use blocking libpq operations.
- There is no reconnection backoff.
- Transaction outcome uncertainty remains unresolved.
- A bounded connection pool has not been implemented.
- Durability under unexpected power failure has not been tested.
- Database latency has not been quantitatively measured.

## 16. Next Steps

Day 8 will add observability to distinguish:

- Application liveness from database readiness
- HTTP request rate from error rate
- Request latency from database-query latency
- CPU usage from time spent waiting
- Connection activity from database availability

The key requirement is to measure failure symptoms without assuming that a running process means a fully functioning service.
