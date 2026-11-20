#!/bin/sh
set -eu

echo "==> Running Aquarium database migrations..."

# Apply user schema and tables first
echo "==> Applying sql/users.sql..."
psql -v ON_ERROR_STOP=1 --username "${POSTGRES_USER}" --dbname "${POSTGRES_DB}" -f /sql/users.sql

# Apply task schema, tables, views, and triggers next (depends on user_schema.tokens)
echo "==> Applying sql/Tasks.sql..."
psql -v ON_ERROR_STOP=1 --username "${POSTGRES_USER}" --dbname "${POSTGRES_DB}" -f /sql/Tasks.sql

# Apply comments schema next (depends on task_schema.tasks and user_schema.tokens)
echo "==> Applying sql/comments.sql..."
psql -v ON_ERROR_STOP=1 --username "${POSTGRES_USER}" --dbname "${POSTGRES_DB}" -f /sql/comments.sql

echo "==> Aquarium database initialized successfully."
