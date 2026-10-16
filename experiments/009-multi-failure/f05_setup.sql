
-- F05: isolated counter and idempotency experiments.
-- Does not modify the existing production counters table.

CREATE TABLE IF NOT EXISTS f05_counters (
    name TEXT PRIMARY KEY,
    value BIGINT NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS f05_idempotency (
    request_id TEXT PRIMARY KEY,
    counter_name TEXT NOT NULL,
    resulting_value BIGINT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

INSERT INTO f05_counters (name, value)
VALUES ('unsafe', 0), ('safe', 0)
ON CONFLICT (name) DO NOTHING;
