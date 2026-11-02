CREATE SCHEMA IF NOT EXISTS task_schema;

CREATE TABLE IF NOT EXISTS task_schema.tasks (
    id BIGSERIAL PRIMARY KEY,
    title TEXT NOT NULL,
    description TEXT NOT NULL,
    author_id BIGINT NOT NULL REFERENCES user_schema.users (id) ON DELETE RESTRICT,
    checker_id BIGINT REFERENCES user_schema.users (id) ON DELETE SET NULL,
    status SMALLINT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP,
    due_date TIMESTAMPTZ
);

CREATE TABLE IF NOT EXISTS task_schema.task_workers (
    task_id BIGINT NOT NULL REFERENCES task_schema.tasks (id) ON DELETE CASCADE,
    worker_id BIGINT NOT NULL REFERENCES user_schema.users (id) ON DELETE RESTRICT,
    PRIMARY KEY (task_id, worker_id)
);

CREATE INDEX IF NOT EXISTS idx_task_workers_worker_id ON task_schema.task_workers (worker_id);

CREATE OR REPLACE VIEW task_schema.task_cache_view AS
SELECT
    t.id,
    COALESCE(ARRAY_AGG(tw.worker_id) FILTER (WHERE tw.worker_id IS NOT NULL), ARRAY[]::BIGINT[]) AS workers,
    COALESCE(t.checker_id, 0) AS checker_id,
    t.status,
    t.created_at,
    t.updated_at
FROM
    task_schema.tasks t
    LEFT JOIN task_schema.task_workers tw ON t.id = tw.task_id
GROUP BY
    t.id;

CREATE OR REPLACE FUNCTION task_schema.touch_task_updated_at ()
    RETURNS TRIGGER
    AS $$
BEGIN
    UPDATE
        task_schema.tasks
    SET
        updated_at = CURRENT_TIMESTAMP
    WHERE
        id = COALESCE(NEW.task_id, OLD.task_id);
    RETURN NULL;
END;
$$
LANGUAGE plpgsql;

CREATE TRIGGER trg_task_workers_touch_task
    AFTER INSERT OR UPDATE OR DELETE ON task_schema.task_workers
    FOR EACH ROW
    EXECUTE FUNCTION task_schema.touch_task_updated_at ();
