CREATE SCHEMA IF NOT EXISTS user_schema;

CREATE TABLE IF NOT EXISTS user_schema.tokens (
    id bigserial PRIMARY KEY,
    key text NOT NULL UNIQUE,
    name varchar(100) NOT NULL,
    scopes text[] NOT NULL,
    updated timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP
);
