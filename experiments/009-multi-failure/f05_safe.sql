
\set ON_ERROR_STOP on

-- Reset only F05 test state.
TRUNCATE TABLE f05_idempotency;

UPDATE f05_counters
SET value = 0
WHERE name = 'safe';

\echo '=== First execution: f05-request-001 ==='

BEGIN;

-- Atomically claim the logical operation.
INSERT INTO f05_idempotency (
    request_id,
    counter_name,
    resulting_value
)
VALUES (
    'f05-request-001',
    'safe',
    -1
)
ON CONFLICT (request_id) DO NOTHING;

-- This first execution owns the new request ID.
UPDATE f05_counters
SET value = value + 1
WHERE name = 'safe'
RETURNING value;

-- Store the committed operation's result.
UPDATE f05_idempotency
SET resulting_value = (
    SELECT value FROM f05_counters WHERE name = 'safe'
)
WHERE request_id = 'f05-request-001';

COMMIT;

\echo '=== Simulated lost acknowledgement ==='
\echo 'The first transaction committed, but the client did not receive confirmation.'

\echo '=== Retry of the SAME logical operation ==='

BEGIN;

-- The existing request ID causes this claim to do nothing.
INSERT INTO f05_idempotency (
    request_id,
    counter_name,
    resulting_value
)
VALUES (
    'f05-request-001',
    'safe',
    -1
)
ON CONFLICT (request_id) DO NOTHING;

-- For an existing request ID, do NOT execute another increment.
-- Instead return the previously persisted result.
SELECT request_id, counter_name, resulting_value
FROM f05_idempotency
WHERE request_id = 'f05-request-001';

COMMIT;

\echo '=== Final counter state ==='

SELECT name, value
FROM f05_counters
WHERE name = 'safe';

\echo '=== Final idempotency record ==='

SELECT request_id, resulting_value
FROM f05_idempotency
WHERE request_id = 'f05-request-001';
