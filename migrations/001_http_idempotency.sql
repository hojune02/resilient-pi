CREATE TABLE IF NOT EXISTS http_idempotency (
    request_key VARCHAR(128) PRIMARY KEY,
    operation TEXT NOT NULL,
    response_code INTEGER NOT NULL,
    response_body TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
