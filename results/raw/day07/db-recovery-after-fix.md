# P05 — PostgreSQL Connection Recovery: After Fix

## Hypothesis

Replacing an invalid PostgreSQL connection before executing a new database operation allows the C++ service to recover from a PostgreSQL restart without restarting the application process.

## Fault injection

1. Start the persistent C++ HTTP server with PostgreSQL available.
2. Verify `GET /counter` returns HTTP 200 and counter value 5.
3. Stop PostgreSQL using `sudo systemctl stop postgresql@16-main`.
4. Verify `GET /counter` returns HTTP 503.
5. Verify `GET /health` continues returning HTTP 200.
6. Restart PostgreSQL using `sudo systemctl start postgresql@16-main`.
7. Verify `GET /counter` returns HTTP 200 and counter value 5.
8. Verify `POST /counter/increment` returns HTTP 200 and counter value 6.

## Observations

- The C++ HTTP service remained available during the database outage.
- Database-dependent operations failed with HTTP 503 while PostgreSQL was unavailable.
- After PostgreSQL restarted, the application successfully re-established database connectivity.
- Previously committed counter state was retained.
- A subsequent database write completed successfully.

## Conclusion

The connection-recovery change resolved the previously reproduced persistent-connection failure for the tested sequence.

## Limitations

- Reconnection attempts are not rate-limited or subject to application-level backoff.
- Recovery latency was not measured.
- Behavior during connection loss at COMMIT remains unverified.
- Transaction retries are not idempotent by default.
- The application currently uses one PostgreSQL connection and one HTTP request-handling thread.
