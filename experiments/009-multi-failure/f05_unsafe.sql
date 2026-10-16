
\set ON_ERROR_STOP on

-- Reset only the disposable test counter.
UPDATE f05_counters
SET value = 0
WHERE name = 'unsafe';

\echo '=== First execution ==='

BEGIN;

UPDATE f05_counters
SET value = value + 1
WHERE name = 'unsafe'
RETURNING value;

COMMIT;

\echo '=== Simulated lost acknowledgement ==='
\echo 'The database committed, but the logical client assumes failure.'

\echo '=== Retry of the same logical operation ==='

BEGIN;

UPDATE f05_counters
SET value = value + 1
WHERE name = 'unsafe'
RETURNING value;

COMMIT;

\echo '=== Final counter value ==='

SELECT name, value
FROM f05_counters
WHERE name = 'unsafe';
