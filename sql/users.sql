CREATE SCHEMA IF NOT EXISTS user_schema;

CREATE TABLE IF NOT EXISTS user_schema.tokens (
    id BIGSERIAL PRIMARY KEY,
    key TEXT NOT NULL UNIQUE DEFAULT gen_random_uuid ()::TEXT,
    name VARCHAR(100) NOT NULL,
    scopes TEXT[] NOT NULL DEFAULT '{}',
    updated TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    admin BOOLEAN NOT NULL DEFAULT FALSE
);

CREATE OR REPLACE FUNCTION user_schema.set_updated_at ()
    RETURNS TRIGGER
    AS $$
BEGIN
    NEW.updated = CURRENT_TIMESTAMP;
    RETURN NEW;
END;
$$
LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_tokens_set_updated_at ON user_schema.tokens;
CREATE TRIGGER trg_tokens_set_updated_at
    BEFORE UPDATE ON user_schema.tokens
    FOR EACH ROW
    EXECUTE FUNCTION user_schema.set_updated_at ();
