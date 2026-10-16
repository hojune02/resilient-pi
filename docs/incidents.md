
# Resilient Pi — Incident Reports

## Incident F01: Application Process Termination and Automatic Recovery

**Date:** 2026-10-16  
**Experiment:** Day 11 — Multi-Failure Injection  
**Status:** Recovered automatically  
**Affected component:** `resilient-pi.service`  
**Application:** V1 single-threaded C++ HTTP server  
**Endpoint:** `http://127.0.0.1:8080/`  
**Failure mechanism:** SIGKILL  
**Recovery mechanism:** systemd `Restart=on-failure`

### 1. Objective

Verify whether systemd automatically detects and recovers from an unexpected C++ server process termination.

Measure:

- Fault detection latency
- User-visible HTTP availability
- Application recovery latency
- Process identity before and after recovery
- Potential data loss
- Whether manual intervention is required

### 2. Hypothesis

Sending SIGKILL to the HTTP server process will cause immediate application unavailability.

Because `resilient-pi.service` is configured with automatic restart on failure and a two-second restart delay, systemd should launch a replacement process without manual intervention.

HTTP requests should fail temporarily and then succeed after the new process begins listening on port 8080.

### 3. Experimental Setup

The systemd-managed application was running on the Raspberry Pi.

The experiment used two SSH terminals:

**Terminal A:** Continuous HTTP availability monitoring using `probe.py`.

**Terminal B:** Process inspection, fault injection, and systemd journal collection.

Probe configuration:

```bash
python3 experiments/009-multi-failure/probe.py \
  --url http://127.0.0.1:8080/ \
  --output results/raw/day11/f01-process-kill.csv \
  --duration 30
```

The probe used:

- Polling interval: 200 ms plus request execution time
- Per-request timeout: 1 second
- HTTP method: GET
- Connection policy: `Connection: close`
- Monotonic timing for request duration
- UTC timestamps for observations

The application was supervised using:

```ini
Restart=on-failure
RestartSec=2
```

### 4. Fault Injection

Before injection:

```text
MainPID=4630
NRestarts=0
```

Fault injection:

```bash
PID=$(systemctl show -P MainPID resilient-pi)
sudo kill -KILL "$PID"
```

Recorded injection timestamp:

```text
2026-10-16T19:16:40.571388826+09:00
```

Immediately after injection:

```text
MainPID=0
NRestarts=0
```

This immediate observation captured the interval after process termination but before systemd had completed restarting the service.

### 5. Detection and Recovery Timeline

All timestamps below are in Korea Standard Time (UTC+09:00).

| Event | Timestamp | Observation |
|---|---|---|
| Last successful HTTP probe before fault | 19:16:40.462034 | HTTP 200 |
| SIGKILL injected | 19:16:40.571389 | PID 4630 targeted |
| First failed HTTP probe | 19:16:40.665331 | URLError |
| Last failed HTTP probe | 19:16:42.684084 | URLError |
| First successful HTTP probe after recovery | 19:16:42.886266 | HTTP 200 |
| systemd service restart | 19:16:42 | New server process started |
| New process verified | After restart | PID 49557, active |

Systemd journal excerpts:

```text
Oct 16 19:16:40 resilient-pi systemd[1]:
resilient-pi.service: Main process exited,
code=killed, status=9/KILL

Oct 16 19:16:40 resilient-pi systemd[1]:
resilient-pi.service: Failed with result 'signal'.

Oct 16 19:16:42 resilient-pi systemd[1]:
resilient-pi.service: Scheduled restart job,
restart counter is at 1.

Oct 16 19:16:42 resilient-pi systemd[1]:
Started resilient-pi.service - Resilient Pi C++ HTTP Server.
```

The original process PID 4630 was replaced by PID 49557.

### 6. Measured Recovery Performance

**Detection latency — injection to first failed probe:**

```text
19:16:40.665331 - 19:16:40.571389
= approximately 0.094 seconds
```

**Observed recovery latency — injection to first successful probe:**

```text
19:16:42.886266 - 19:16:40.571389
= approximately 2.315 seconds
```

**First observed failure to first observed recovery:**

```text
19:16:42.886266 - 19:16:40.665331
= approximately 2.221 seconds
```

**Failed HTTP probes:** 11 consecutive observations.

All 11 failed probes were reported as `URLError`, without an HTTP status code.

The recovered process returned HTTP 200 and continued responding successfully.

These are probe-observed measurements, not exact kernel-level downtime boundaries.

### 7. User-Visible Impact

During the recovery interval:

- The HTTP endpoint was temporarily unavailable.
- Clients experienced connection-level errors.
- The application did not return an HTTP 503 response during the process outage.
- The probe resumed receiving HTTP 200 responses after systemd restarted the server.

The failure resulted in temporary unavailability rather than graceful degradation.

The observed service recovery required no manual restart command.

### 8. Root Cause

The application process was deliberately terminated using SIGKILL.

SIGKILL prevents normal application-level signal handling and cleanup.

Systemd observed the abnormal process exit and applied its configured restart policy.

The incident was therefore an injected application-process failure, not an unexpected application crash caused by a software defect.

### 9. Recovery Mechanism

The recovery sequence was:

```text
SIGKILL to PID 4630
        |
        v
Application process terminates
        |
        v
systemd detects abnormal exit
        |
        v
Restart policy activates
        |
        v
Configured restart delay
        |
        v
New application process starts
        |
        v
PID 49557 listens on port 8080
        |
        v
HTTP requests succeed again
```

The observed recovery time of approximately 2.315 seconds is consistent with the configured two-second restart delay plus additional process startup and probe-detection time.

The experiment demonstrates recovery through process supervision rather than recovery logic implemented inside the C++ application itself.

### 10. Data Integrity Assessment

The V1 HTTP server returns a static response and does not perform PostgreSQL writes.

Therefore:

- The experiment did not involve a persistent database transaction.
- No database rollback or recovery operation was required.
- No persistent application-state inconsistency was observed.
- Data durability under interruption was not tested by F01.
- Requests in flight at the moment of SIGKILL were not individually tracked.

The result must not be generalized into a claim that arbitrary database-backed requests are safe against process termination.

### 11. Retry Behavior

The probe repeatedly sent independent GET requests after each failure.

It was not an application-level retry of the same transaction.

Because the tested endpoint was read-only and returned static content, repeated requests did not introduce duplicate persistent work.

The experiment does not establish that automatic retries are safe for non-idempotent operations such as `POST /counter/increment`.

For database-backed operations, an interrupted request may have an uncertain commit outcome.

Such operations should not be blindly retried without an idempotency mechanism.

### 12. Limitations

1. Only one SIGKILL fault was injected.
2. Recovery timing depends on the probe interval and request execution time.
3. Probe timestamps were generated before request execution.
4. The experiment used localhost rather than a remote client.
5. Only a static, database-independent endpoint was tested.
6. The effect on requests already in flight was not measured.
7. The systemd restart delay was configured beforehand rather than optimized experimentally.
8. No alternative recovery policy was compared.
9. No sustained load was applied during recovery.
10. The experiment does not prove that every future application crash will recover within 2.315 seconds.

### 13. Engineering Findings

**F01-F1: Process liveness depends on the process lifecycle, not merely network connectivity.**

When the C++ process terminated, the HTTP endpoint stopped responding even though the operating system continued running.

**F01-F2: systemd supervision restored application availability automatically.**

The service recovered through systemd's restart mechanism, without manual intervention.

**F01-F3: Detection latency and recovery latency are different measurements.**

The first failure was observed approximately 94 ms after fault injection, while service recovery was observed approximately 2.315 seconds after injection.

**F01-F4: Monitoring resolution affects measured downtime.**

The probe ran at approximately 200 ms intervals. The exact time at which the new server began accepting connections may precede the first successful observation.

**F01-F5: Process recovery does not guarantee request-level correctness.**

Although the static HTTP endpoint recovered, the experiment did not validate persistent state or non-idempotent operations.

### 14. Potential Improvements

- Add explicit liveness and readiness endpoints to the supervised server.
- Expose restart counts and application uptime as observability metrics.
- Establish an availability SLO and recovery-time objective.
- Test repeated crashes and systemd restart-rate limiting.
- Test process termination while requests are actively being handled.
- Integrate application supervision with the PostgreSQL-backed implementation.
- Evaluate graceful shutdown behavior separately from SIGKILL.
- Test idempotent request recovery for database-backed operations.

Reducing `RestartSec` may reduce downtime, but should not be done without considering crash loops and restart amplification.

### 15. Evidence

```text
results/raw/day11/f01-process-kill.csv
results/raw/day11/f01-fault-time.txt
results/raw/day11/f01-systemd-journal.txt
```

Related source and configuration:

```text
experiments/009-multi-failure/probe.py
server/main.cpp
/etc/systemd/system/resilient-pi.service
```

### 16. Final Assessment

**Incident outcome: Automatic recovery successful.**

The systemd-managed C++ HTTP server was deliberately terminated using SIGKILL.

The probe observed 11 consecutive failed HTTP requests, followed by successful recovery approximately 2.315 seconds after fault injection.

systemd restarted the application from PID 4630 to PID 49557.

The experiment demonstrates that process supervision can automatically restore basic HTTP availability after an unexpected application termination.

Further experiments are required to establish recovery correctness for database-backed, state-changing requests.


---

## Incident F02: PostgreSQL Dependency Failure and Automatic Reconnection

**Date:** 2026-10-16  
**Experiment:** Day 11 — Multi-Failure Injection  
**Status:** Recovered  
**Affected dependency:** PostgreSQL 16, cluster `16/main`  
**Application:** Observable C++ HTTP server, port 8082  
**Fault mechanism:** Controlled PostgreSQL service stop  
**Recovery mechanism:** PostgreSQL restart and application-level connection recovery

### 1. Objective

Investigate how a running HTTP application behaves when its database dependency becomes unavailable.

Measure dependency failure detection, user-visible behavior, recovery time, and persistent-state availability.

### 2. Hypothesis

When PostgreSQL stops:

- The HTTP application's liveness endpoint should remain responsive.
- Database readiness should fail with HTTP 503.
- Database-dependent requests should return HTTP 503.
- Restarting PostgreSQL should allow the application to reconnect without restarting its own process.

### 3. Experimental Setup

Two independent HTTP probes were configured:

| Probe | Endpoint | Purpose |
|---|---|---|
| Health | `GET /health` | Application liveness |
| Readiness | `GET /ready` | Database dependency readiness |

Each probe ran for approximately 60 seconds with a nominal polling interval of 200 ms and a per-request timeout of one second.

Fault injection:

```bash
sudo systemctl stop postgresql@16-main
```

Recovery:

```bash
sudo systemctl start postgresql@16-main
```

The C++ application was not intentionally restarted.

### 4. Observed Timeline

All timestamps are in Korea Standard Time (UTC+09:00).

| Event | Timestamp |
|---|---|
| Stop-command timestamp | 19:25:59.625264 |
| First readiness HTTP 503 | 19:26:06.033855 |
| Last readiness HTTP 503 | 19:26:18.038006 |
| First recovered readiness HTTP 200 | 19:26:18.240517 |
| Timestamp recorded after start command | 19:26:20.396731 |

The timestamp labeled as the PostgreSQL restart command time was recorded after `systemctl start` returned. It is not the timestamp when the restart command was issued.

### 5. Quantitative Results

| Measurement | Result |
|---|---:|
| Readiness probes | 295 |
| Readiness HTTP 503 responses | 60 |
| Liveness probes | 295 |
| Liveness failures | 0 |
| Fault command to first observed 503 | 6.409 seconds |
| First 503 to first subsequent HTTP 200 | 12.207 seconds |
| Fault command to first recovered HTTP 200 | 18.615 seconds |

The exact database unavailability interval was not measured directly.

These durations include service transition time, probe sampling delay, and, for the final recovery measurement, the deliberate outage period.

They should not be interpreted as intrinsic PostgreSQL restart latency or the time spent reconnecting through libpq.

### 6. User-Visible Impact

During the database outage:

```text
GET /health  -> HTTP 200
GET /ready   -> HTTP 503
GET /counter -> HTTP 503 (manually tested)
```

The HTTP application continued accepting requests.

Dependency-related functionality became unavailable, but application liveness remained intact.

After PostgreSQL recovered:

```text
GET /health  -> HTTP 200
GET /ready   -> HTTP 200
GET /counter -> HTTP 200, value 16
```

The PostgreSQL cluster was confirmed online after restart.

### 7. Recovery Mechanism

The C++ application retains a PostgreSQL connection through libpq.

Following PostgreSQL termination, this connection can become unusable.

The previously implemented `ensure_connected()` mechanism replaces an invalid connection when another database operation is attempted.

A successful `/ready` check performs a database round trip, confirming more than the existence of the HTTP process.

The experiment demonstrated that database functionality recovered without an intentional restart of the application.

### 8. Data Consistency

After recovery, the database-backed counter returned:

```text
16
```

The value was consistent with the previously observed committed counter state.

However, the supplied experiment logs do not contain a separate timestamped pre-fault SQL value, and no writes were attempted during the outage.

Therefore, the experiment demonstrates post-recovery readability of the committed counter, but does not test data loss from an interrupted transaction or consistency under concurrent writes.

### 9. Retry Behavior

The readiness probe repeatedly issued independent read-only checks.

These are not retries of a failed state-changing transaction.

The experiment did not implement automatic replay of failed increments.

This distinction matters because an application can lose its connection during COMMIT while PostgreSQL may still have committed the transaction.

Blindly retrying such an increment could duplicate work.

### 10. Root Cause and Recovery Assessment

The dependency failure was deliberately injected by stopping PostgreSQL.

The application remained alive but could not complete database-dependent operations.

Restoring PostgreSQL enabled subsequent requests to succeed through application-level connection recovery.

This confirms a limited form of graceful degradation:

- The HTTP server remained available.
- Database-independent liveness checks continued working.
- Database-dependent operations failed explicitly instead of returning fabricated data.
- Functionality returned after dependency recovery.

### 11. Limitations

1. One controlled PostgreSQL outage was tested.
2. The exact instant PostgreSQL became unavailable was not recorded.
3. The PostgreSQL start-command timestamp was recorded after command completion.
4. The probes used localhost, not a remote client.
5. The observable HTTP server is single-threaded.
6. Database writes were not performed during failure.
7. Mid-COMMIT failure and ambiguous transaction outcomes were not tested.
8. Connection timeout and retry-backoff behavior were not benchmarked.
9. No independent application PID comparison was included in the supplied results.
10. The experiment does not determine recovery performance under high concurrent load.

### 12. Improvements

- Record timestamps immediately before and after every fault and recovery command.
- Monitor PostgreSQL service state alongside application readiness.
- Add database reconnect-attempt and failure metrics.
- Use bounded reconnection backoff where appropriate.
- Avoid retrying non-idempotent transactions without deduplication.
- Add request-level idempotency keys for state-changing operations.
- Test dependency failure during a controlled transaction.

### 13. Evidence

- `results/raw/day11/f02-health.csv`
- `results/raw/day11/f02-ready.csv`
- `results/raw/day11/f02-fault-time.txt`
- `results/raw/day11/f02-restart-command-time.txt`
- `results/raw/day11/f02-postgresql-journal.txt` (if saved)

### 14. Conclusion

**Incident outcome: Database dependency recovery successful.**

The application remained responsive while PostgreSQL was unavailable.

The readiness probe recorded 60 HTTP 503 responses, while all 295 liveness probes returned HTTP 200.

The first recovered readiness response occurred approximately 18.615 seconds after the recorded stop-command timestamp.

The experiment demonstrates the value of separating application liveness from dependency readiness and of recovering invalid database connections without automatically retrying state-changing transactions.

---

## Incident F03: Network-Path Failure and Automatic Connectivity Recovery

**Date:** 2026-10-16  
**Experiment:** Day 11 — Multi-Failure Injection  
**Status:** Recovered automatically  
**Affected path:** Arch Linux `10.42.0.1` → Raspberry Pi `10.42.0.2:8080`  
**Fault mechanism:** nftables packet drop  
**Recovery mechanism:** Timed firewall-rule removal through systemd

### 1. Objective

Investigate how an HTTP client behaves when the network path to an otherwise running application becomes unavailable.

Measure detection latency, request timeout behavior, client-visible failures, network recovery, and the effectiveness of automatic fault cleanup.

### 2. Hypothesis

Dropping incoming TCP packets from Arch Linux to the Raspberry Pi's HTTP port should cause client requests to time out rather than immediately fail with connection-refused errors.

The temporary firewall rule should be removed automatically, restoring connectivity without restarting the HTTP application.

### 3. Experimental Setup

The Raspberry Pi ran the systemd-managed V1 C++ HTTP server on TCP port 8080.

The Arch Linux machine continuously probed:

`http://10.42.0.2:8080/`

Probe parameters:

| Parameter | Value |
|---|---|
| Monitoring host | Arch Linux |
| Server | Raspberry Pi |
| Polling interval | 200 ms after each request |
| Request timeout | 1 second |
| Total probe duration | 45 seconds |
| Protocol | HTTP over TCP |
| Expected response | HTTP 200 |

The temporary firewall rule was placed in a separate nftables table, avoiding modifications to the existing Tailscale-managed chains.

### 4. Fault Injection

Fault preparation:

```bash
sudo nft add table inet resilient_pi_f03
sudo nft 'add chain inet resilient_pi_f03 input { type filter hook input priority -10; policy accept; }'
```

A systemd cleanup timer was scheduled before installing the blocking rule.

The blocking rule matched:

- Incoming interface: `eth0`
- Source IP: `10.42.0.1`
- Destination IP: `10.42.0.2`
- Destination TCP port: `8080`
- Action: `drop`

Recorded fault-procedure timestamp:

`2026-10-16T19:40:11.178656428+09:00`

The timestamp was captured before installation of the firewall rule, so it does not precisely represent the moment when packet dropping began.

### 5. Firewall Evidence

During the experiment:

```text
table inet resilient_pi_f03 {
    chain input {
        type filter hook input priority filter - 10;
        policy accept;
        iifname "eth0" ip saddr 10.42.0.1
        ip daddr 10.42.0.2 tcp dport 8080
        counter packets 12 bytes 720 drop
    }
}
```

The rule's packet counter demonstrated that matching packets were dropped.

This directly supports the interpretation that the observed client failures were caused by the injected network restriction.

### 6. Observed Timeline

All timestamps are in Korea Standard Time (UTC+09:00).

| Event | Timestamp |
|---|---|
| Fault-procedure timestamp | 19:40:11.178656 |
| Last successful HTTP probe | 19:40:11.241004 |
| First failed HTTP probe | 19:40:11.442680 |
| Last failed HTTP probe | 19:40:24.661492 |
| Firewall cleanup journal entry | 19:40:24 |
| First recovered HTTP 200 | 19:40:25.863655 |

The cleanup journal confirmed that the deletion service executed and exited successfully.

A subsequent nftables inspection reported that the temporary table no longer existed.

### 7. Quantitative Measurements

| Measurement | Observed result |
|---|---:|
| Failed HTTP probes | 12 |
| Approximate failed-request duration | 1,001–1,002 ms |
| Fault-procedure timestamp to first observed failure | 0.264 seconds |
| First failed probe to first recovered HTTP 200 | 14.421 seconds |
| Fault-procedure timestamp to first recovered HTTP 200 | 14.685 seconds |
| Dropped packets observed | 12 |

The 14.685-second measurement includes the delay before the rule was actually installed, the intentional network restriction, request timeouts, and probe sampling delay.

It is not the exact duration for which the firewall rule existed.

### 8. User-Visible Impact

During the network restriction, the Arch Linux client experienced request failures recorded as `URLError`.

Each failed request consumed approximately one second, matching the probe's explicit request timeout.

The client received no HTTP error status from the application because the requests failed at the network layer.

Following firewall cleanup, the client again received HTTP 200 responses with normal request latency.

The experiment did not capture a simultaneous local HTTP probe on the Raspberry Pi during the restriction. Therefore, uninterrupted application availability during the fault was expected from the narrowly scoped rule but not independently measured in this run.

### 9. Recovery Mechanism

The fault was temporary because a systemd timer was configured to remove the nftables table automatically.

Cleanup journal:

```text
Oct 16 19:40:24 resilient-pi systemd[1]:
Started resilient-pi-f03-cleanup.service

Oct 16 19:40:24 resilient-pi systemd[1]:
resilient-pi-f03-cleanup.service: Deactivated successfully.
```

After cleanup, remote HTTP requests succeeded again.

The recovery mechanism was restoration of the network path, not application restart or database reconnection.

### 10. Retry and Timeout Analysis

The probe sent repeated independent GET requests.

Because the firewall used `drop`, connections waited for the configured one-second timeout rather than failing immediately.

The effective interval between failed probes was approximately 1.2 seconds: one second spent waiting, followed by the 200 ms polling delay.

This illustrates why timeout selection affects both failure latency and the rate of subsequent requests.

An overly long timeout increases client waiting time. An overly short timeout may fail requests during temporary network delays.

Aggressive retries can amplify traffic and load during partial recovery.

The experiment did not evaluate exponential backoff, jitter, or concurrent retry amplification.

### 11. Data Integrity

The tested endpoint returned static content and performed no persistent writes.

No transaction was intentionally interrupted.

The experiment therefore does not establish data integrity under network failure during a PostgreSQL commit or other state-changing operation.

### 12. Engineering Findings

**F03-F1: Application health and client reachability are distinct.**

A network-path failure can prevent clients from reaching an application without requiring the application process to terminate.

**F03-F2: Failure type determines client-visible behavior.**

A process crash can produce immediate connection errors, whereas silent packet dropping can produce timeout delays.

**F03-F3: Client timeouts determine how quickly failures become visible.**

The probe's one-second timeout controlled the duration of each failed request.

**F03-F4: Automatic cleanup can reduce fault-injection risk.**

The systemd timer removed the temporary firewall rule successfully and restored connectivity.

**F03-F5: Recovery measurements must include sampling uncertainty.**

The first successful probe was observed after rule cleanup, but the exact instant network connectivity returned was not measured.

### 13. Limitations

- Only one client-server network path was tested.
- No simultaneous localhost health probe was captured.
- The exact rule-installation timestamp was not recorded.
- A one-second request timeout reduced the number of probes during the outage.
- No in-flight database transaction was tested.
- No packet loss rate or round-trip latency impairment was introduced beyond complete packet dropping.
- The experiment did not evaluate retry backoff or circuit breakers.
- The reported recovery time includes the intentional fault duration and client-probe delay.

### 14. Recommendations

1. Use explicit connection and request timeouts.
2. Distinguish application liveness from external network reachability.
3. Add remote availability monitoring alongside local service monitoring.
4. Apply bounded retries with backoff and jitter where appropriate.
5. Avoid automatically retrying non-idempotent operations without deduplication.
6. Continue to use automatic rollback mechanisms for network fault injection.

### 15. Evidence

- `results/raw/day11/f03-fault-time.txt`
- `results/raw/day11/f03-network.csv` (copied from Arch Linux)
- systemd cleanup journal
- nftables packet-counter observation
- Arch Linux post-recovery HTTP 200 response

### 16. Conclusion

**Incident outcome: Network-path recovery successful.**

A temporary nftables rule caused 12 consecutive failed HTTP probes from Arch Linux to the Raspberry Pi.

The firewall recorded 12 dropped packets, and the client experienced approximately one-second request timeouts.

After automatic removal of the temporary firewall table, the client resumed receiving HTTP 200 responses.

The experiment demonstrated that network reachability failures require different detection and recovery strategies from application-process failures.

---

## Incident F04: PostgreSQL Backend Termination and Connection Recovery

**Date:** 2026-10-16  
**Experiment:** Day 11 — Multi-Failure Injection  
**Status:** Recovery verified  
**Affected component:** Dedicated PostgreSQL backend for F04  
**Application:** Disposable C++ observable server, port 8084  
**Failure mechanism:** `pg_terminate_backend()`  
**Recovery mechanism:** libpq connection replacement through `ensure_connected()`

### 1. Objective

Determine whether the C++ application can recover from unexpected termination of an established PostgreSQL connection without restarting either the application or the PostgreSQL service.

The experiment isolates a single database session rather than stopping the entire database cluster.

### 2. Experimental Setup

A disposable copy of `observable_server.cpp` was compiled as `build/day11/f04-server`.

The application connected over TCP to PostgreSQL on `127.0.0.1:5432`, using the database `resilient_pi` and PostgreSQL role `hojune`.

The disposable application listened on `127.0.0.1:8084`.

The connection was identified using Linux socket inspection and PostgreSQL's `pg_stat_activity` view.

### 3. Fault Injection

Initial application process:

```text
F04 PID: 52882
```

Initial TCP connection:

```text
127.0.0.1:52596 -> 127.0.0.1:5432
```

PostgreSQL backend:

```text
PID: 52884
User: hojune
Client address: 127.0.0.1
Client port: 52596
State: idle
```

Recorded fault-command timestamp:

```text
2026-10-16T20:09:06.101643608+09:00
```

The database backend was terminated using `pg_terminate_backend()` after matching the client's TCP port to the selected PostgreSQL session.

PostgreSQL returned `t`, indicating the termination request was accepted.

### 4. Observed Recovery

After fault injection:

```text
PostgreSQL service: active
F04 service: active
GET /ready: HTTP 200
GET /counter: HTTP 200
Counter value: 16
```

The F04 application process remained PID 52882.

A subsequent socket inspection showed:

```text
127.0.0.1:42012 -> 127.0.0.1:5432
```

The new PostgreSQL backend was PID 53129.

This demonstrates that the application established a replacement database connection without restarting its process.

### 5. Root Cause and Recovery Mechanism

The selected PostgreSQL backend was deliberately terminated.

This invalidated the existing database session associated with the disposable C++ server.

The application implements reconnection through `ensure_connected()`, which checks the libpq connection status and replaces an invalid connection using `PQconnectdb()`.

The observed new TCP connection is consistent with this recovery mechanism.

The experiment did not establish whether the first request after termination failed before reconnection occurred.

### 6. Data Integrity

The counter returned `16` after recovery.

No state-changing request was deliberately executed during backend termination.

Therefore, the experiment supports successful read-only recovery but does not establish correctness for transactions interrupted during COMMIT.

### 7. Recovery Measurements

Two independent probes monitored application liveness (`GET /health`) and database readiness (`GET /ready`) with approximately 200 ms polling intervals.

The PostgreSQL backend termination command was timestamped at `2026-10-16T20:09:06.101643608+09:00`.

| Measurement | Result |
|---|---:|
| Application PID before and after | 52882 |
| Original PostgreSQL backend PID | 52884 |
| Replacement PostgreSQL backend PID | 53129 |
| First failed readiness probe | 20:09:06.327391 KST |
| First recovered readiness probe | 20:09:06.531070 KST |
| Fault command to first HTTP 503 | 225.7 ms |
| Fault command to first recovered HTTP 200 | 429.4 ms |
| First observed HTTP 503 to recovered HTTP 200 | 203.7 ms |
| Failed readiness probes | 1 |
| Failed liveness probes | 0 |
| HTTP latency of recovered readiness probe | 55.658 ms |

The readiness probe returned exactly one HTTP 503 response before recovering. All subsequent readiness probes in the captured window returned HTTP 200.

The liveness probe continued returning HTTP 200 throughout the experiment.

These results demonstrate a transient database-readiness failure followed by automatic application-level reconnection. They do not establish the exact instant the backend terminated or the precise duration of database unavailability.

### 7.1 Interpretation

The first readiness request after backend termination failed because the existing database connection had become invalid.

A subsequent readiness request successfully established or used a replacement connection, demonstrated independently by the new PostgreSQL backend PID and TCP connection.

The application did not restart, and PostgreSQL remained online throughout the test.

The recovered readiness request took 55.658 ms, compared with approximately 3–5 ms for typical healthy readiness requests near the fault. This suggests additional recovery overhead but does not isolate the PostgreSQL connection-establishment time.

The experiment confirms that **application liveness and dependency readiness are distinct**: the HTTP process continued responding even while one readiness request failed.

### 8. Engineering Findings

**F04-F1: A PostgreSQL connection can fail independently of the database service.**

The selected backend was terminated while PostgreSQL remained operational.

**F04-F2: Application-level reconnection can recover a failed database session.**

The application maintained its PID and established a replacement TCP connection.

**F04-F3: Successful recovery does not necessarily imply zero failed requests.**

The available evidence confirms the final healthy state but does not show whether transient request failures occurred.

**F04-F4: Connection establishment timeouts differ from query execution timeouts.**

The configured `connect_timeout=2` applies to establishing new connections. This experiment did not measure behavior when the destination is unreachable or when an established query stalls.

### 9. Limitations

- Only one backend was terminated.
- Continuous probe results were not supplied.
- The test did not simulate packet loss or a persistent network partition.
- No state-changing request was interrupted.
- No retry backoff or circuit breaker was exercised.
- The experiment did not measure the time required to establish the replacement connection.

### 10. Conclusion

**Incident outcome: Connection recovery verified.**

Terminating the PostgreSQL backend for the disposable F04 server caused the existing database session to be replaced.

The C++ application remained running, PostgreSQL remained online, and subsequent readiness and counter requests succeeded.

The experiment demonstrates connection recovery after a localized database-session failure, while request-level failure and recovery timing remain unmeasured.

---

## Incident F05: Ambiguous Commit Outcome and Duplicate Transaction Execution

**Date:** 2026-10-16  
**Experiment:** Day 11 — Multi-Failure Injection  
**Status:** Corrected and verified  
**Affected component:** PostgreSQL transaction handling  
**Failure model:** Simulated lost transaction acknowledgement  
**Mitigation:** Transactional idempotency record and request deduplication

### 1. Objective

Investigate whether retrying an operation after losing its acknowledgement can create duplicate database changes.

Demonstrate the difference between a non-idempotent increment and a transaction protected by a persistent request identifier.

### 2. Experimental Setup

Two disposable counters were created in PostgreSQL:

- `unsafe`: incremented without deduplication.
- `safe`: incremented using an idempotency record.

The experiment used:

- `f05_counters(name, value)`
- `f05_idempotency(request_id, counter_name, resulting_value, created_at)`

The existing production `counters` table was not modified.

### 3. Unsafe Retry Experiment

The first transaction incremented the counter from 0 to 1 and committed.

The experiment then simulated a lost acknowledgement by treating the completed transaction as unsuccessful from the client's perspective.

The same logical operation was executed again.

Observed results:

```text
First execution: value = 1
COMMIT

Simulated lost acknowledgement

Retry: value = 2
COMMIT

Final unsafe counter = 2
```

**Result:** One logical operation caused two committed increments.

The database behaved correctly for the two SQL transactions it received; the error arose because the retry had no durable identifier connecting it to the first execution.

### 4. Initial Idempotency Implementation Failure

The first idempotency implementation used a data-modifying CTE to insert an idempotency record and subsequently update that record within one SQL statement.

The counter ended at 1, but the stored result remained the placeholder `-1`.

Observed:

```text
safe counter = 1
resulting_value = -1
UPDATE 0
```

The initial solution prevented duplicate increments but failed to persist the correct response.

This exposed a second correctness requirement: an idempotent operation must not only avoid duplicate effects but also preserve the outcome that subsequent requests should receive.

### 5. Correction

The SQL was revised to execute the claim, increment, and result-storage operations as separate statements inside one PostgreSQL transaction.

First execution:

```text
BEGIN
INSERT 0 1
UPDATE 1
UPDATE 1
COMMIT
```

Retry with the same request ID:

```text
BEGIN
INSERT 0 0
SELECT previously stored result
COMMIT
```

The retry did not execute a second counter increment.

### 6. Final Verification

| Measurement | Unsafe | Safe |
|---|---:|---:|
| Initial counter | 0 | 0 |
| Logical requests | 1 | 1 |
| Simulated executions | 2 | 2 |
| Committed increments | 2 | 1 |
| Final counter | 2 | 1 |

The final idempotency record contained:

```text
request_id: f05-request-001
counter_name: safe
resulting_value: 1
```

Both the counter and stored outcome were verified using a separate database query after the experiment.

### 7. Recovery and Retry Behavior

The unsafe retry duplicated work.

The idempotent retry recognized the previously committed request identifier and returned its stored result without applying another increment.

The idempotency record and counter update were committed together in the corrected first-execution transaction.

However, the demonstration explicitly separated first-execution and retry logic in the SQL script. A production implementation must inspect whether the unique request-ID claim succeeded and select the appropriate branch automatically.

### 8. Engineering Findings

**F05-F1: A failed acknowledgement does not imply a failed transaction.**

The underlying database transaction may have committed before its confirmation was lost.

**F05-F2: Blind retries can duplicate state-changing operations.**

The unsafe counter increased twice for one logical operation.

**F05-F3: Deduplication and result replay are separate correctness requirements.**

The first attempted implementation deduplicated the increment but stored an invalid result.

**F05-F4: Atomic transactions protect related state changes.**

The counter update and idempotency record must succeed or fail together.

**F05-F5: Idempotency requires a stable logical request identity.**

A retried operation must reuse the same request identifier; generating a new identifier for every attempt defeats deduplication.

### 9. Limitations

- A lost acknowledgement was simulated rather than caused by an actual interrupted network connection.
- The experiment did not terminate a database transaction mid-COMMIT.
- The corrected SQL uses manually selected first-execution and retry paths.
- Concurrent duplicate submissions were not tested.
- Reuse of a request ID with different parameters was not tested.
- No production HTTP endpoint currently implements this complete idempotency protocol.
- The experiment does not establish exactly-once network delivery.

### 10. Recommended Application Design

A production increment endpoint should:

1. Accept a unique idempotency key from the client.
2. Atomically attempt to register that key.
3. Execute the database update only when the key is newly claimed.
4. Persist the operation's result within the same transaction.
5. Return the persisted result for duplicate requests.
6. Reject reuse of the same key with conflicting parameters.
7. Define expiration and retention rules for idempotency records.

### 11. Evidence

- `experiments/009-multi-failure/f05_setup.sql`
- `experiments/009-multi-failure/f05_unsafe.sql`
- `experiments/009-multi-failure/f05_safe.sql`
- `results/raw/day11/f05-unsafe.txt`
- `results/raw/day11/f05-safe.txt`

### 12. Conclusion

**Incident outcome: Duplicate work demonstrated; corrected idempotency transaction verified.**

The non-idempotent implementation incremented the counter twice after a simulated lost acknowledgement.

The corrected idempotency demonstration incremented the counter once and stored the correct result, which was returned on retry.

The experiment shows that service availability, retry behavior, and persistent-state correctness must be evaluated separately.