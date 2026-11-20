CREATE TABLE IF NOT EXISTS task_schema.task_comments (
    id BIGSERIAL PRIMARY KEY,
    task_id BIGINT NOT NULL REFERENCES task_schema.tasks (id) ON DELETE CASCADE,
    author_id BIGINT NOT NULL REFERENCES user_schema.tokens (id) ON DELETE RESTRICT,
    content TEXT NOT NULL,
    comment_type SMALLINT NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX IF NOT EXISTS idx_task_comments_task_id ON task_schema.task_comments (task_id);
