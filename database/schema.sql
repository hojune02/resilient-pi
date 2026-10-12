CREATE TABLE IF NOT EXISTS counters (
    name TEXT PRIMARY KEY,
    value BIGINT NOT NULL DEFAULT 0
);

INSERT INTO counters (name, value)
VALUES ('requests', 0)
ON CONFLICT (name) DO NOTHING;
