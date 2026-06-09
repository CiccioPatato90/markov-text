-- ---------- sequences for surrogate ids ----------
CREATE SEQUENCE IF NOT EXISTS seq_vocab    START 1;
CREATE SEQUENCE IF NOT EXISTS seq_doc      START 1;
CREATE SEQUENCE IF NOT EXISTS seq_trainrun START 1;
CREATE SEQUENCE IF NOT EXISTS seq_genrun   START 1;

-- ---------- vocabulary: global token frequencies (powers Zipf + joins) ----------
CREATE TABLE IF NOT EXISTS vocabulary (
    id    INTEGER PRIMARY KEY DEFAULT nextval('seq_vocab'),
    token VARCHAR UNIQUE NOT NULL,
    freq  BIGINT  NOT NULL DEFAULT 0
);

-- ---------- documents: one row per ingested source text ----------
CREATE TABLE IF NOT EXISTS documents (
    id          INTEGER PRIMARY KEY DEFAULT nextval('seq_doc'),
    name        VARCHAR NOT NULL,
    added_at    TIMESTAMP DEFAULT now(),
    token_count BIGINT
);

-- ---------- training_runs: one row per (document, order) training ----------
CREATE TABLE IF NOT EXISTS training_runs (
    id           INTEGER PRIMARY KEY DEFAULT nextval('seq_trainrun'),
    document_id  INTEGER REFERENCES documents(id),
    ord          TINYINT NOT NULL,
    ngrams_added BIGINT,
    ran_at       TIMESTAMP DEFAULT now()
);

-- ---------- transitions: THE model. all orders share this table ----------
CREATE TABLE IF NOT EXISTS transitions (
    ord       TINYINT NOT NULL,                 -- context length
    state_key VARCHAR NOT NULL,                 -- space-joined previous `ord` tokens
    next_id   INTEGER NOT NULL REFERENCES vocabulary(id),
    count     BIGINT  NOT NULL DEFAULT 0,
    PRIMARY KEY (ord, state_key, next_id)       -- also the ON CONFLICT target
);

-- ---------- generation_runs: one row per :gen invocation ----------
CREATE TABLE IF NOT EXISTS generation_runs (
    id              INTEGER PRIMARY KEY DEFAULT nextval('seq_genrun'),
    seed            VARCHAR,
    requested_order TINYINT,
    max_tokens      INTEGER,
    created_at      TIMESTAMP DEFAULT now()
);

-- ---------- generation_steps: one row per generated token ----------
CREATE TABLE IF NOT EXISTS generation_steps (
    run_id         INTEGER REFERENCES generation_runs(id),
    step           INTEGER NOT NULL,
    state_key      VARCHAR,
    order_used     TINYINT,           -- which order actually fired after backoff
    chosen_next_id INTEGER REFERENCES vocabulary(id),
    chosen_prob    DOUBLE,            -- count/total within the firing order's state
    n_candidates   INTEGER,
    PRIMARY KEY (run_id, step)
);
