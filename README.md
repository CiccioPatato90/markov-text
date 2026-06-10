# Markov-DB: a DuckDB-backed multi-order Markov text engine

> **Course context** — Université de Toulouse, M2 CSA 2025–2026, *Embedded Databases* (S. Yin).
> Mini-project: an embedded-database application. This one uses **DuckDB** (embedded, in-process,
> columnar/OLAP) as the home of a **variable-order Markov chain** with **incremental training**,
> **backoff generation**, an interactive **REPL**, and an **OLAP statistics** layer.

This README is the implementation guide; the implementation now exists and follows it
(`markov.c` + `schema.sql` + `train_stage.sql` / `train_order.sql`, ~460 lines total). The old
in-memory C prototype has been retired — see *§9 Migrating from the old prototype*.

---

## 1. What we are building (and why)

A word-level Markov text generator where **the model lives in the database**, not in RAM:

- **Incremental training.** Feed documents one at a time; transition counts accumulate in DuckDB
  across runs and across process restarts. No retraining from scratch, no `MAX_SIZE` cap.
- **Mixed-order model.** Train one document at order 1, another at order 6 — all orders coexist in
  one `transitions` table, distinguished by an `ord` column.
- **Backoff generation.** At each step we use the **longest context we actually have data for**, and
  fall back to a shorter one when the long context is unseen ("stupid backoff").
- **Tokenization in SQL.** DuckDB reads the file and tokenizes with regex + window functions —
  Unicode-aware, lowercased, sentence-segmented. The C layer barely touches the text.
- **REPL + OLAP stats.** Interactively train, generate (seeded continuation), and run analytics
  (perplexity, backoff profile, Zipf, entropy, corpus growth) — every generation step is logged so
  the statistics are real, not hand-waved.

### Why DuckDB (the report's central argument)
The teacher asks whether DuckDB is "too complex." The honest answer is **no, and it's the better
fit here** because the *query* workload is analytical (counting, grouping, ranking, window
functions). The one place DuckDB is weak — high-frequency single-row OLTP upserts — we **design
away** by batching: each document is tokenized and merged into the model in **one set-based
`INSERT ... ON CONFLICT`**, never one row at a time. SQLite would be the textbook choice for a
per-token streaming writer; our batched, analytics-heavy pattern plays to DuckDB's strengths.

---

## 2. Design decisions (settled — put these in the report)

| # | Decision | Choice | Rationale |
|---|----------|--------|-----------|
| 1 | State representation | `state_key` is a space-joined **text** string; `next_id` is **normalized** to `vocabulary.id` | Text state keeps the n-gram SQL simple; normalizing `next` gives real joins for stats without a heavy `state_tokens` bridge table |
| 2 | Token policy | Keep letters + intra-word apostrophes (`[\p{L}][\p{L}']*`); normalize curly `’` to `'` first; drop standalone numbers/punctuation | Preserves "don't", "l'eau" — including with typographic quotes; avoids number noise |
| 3 | Sentence boundaries | Insert `<s>` / `</s>` sentinels per sentence | Generation can *start* and *end* naturally instead of from a random mid-text state |
| 4 | Sampling location | Weighted random sampling done **in SQL** (cumulative-count window) | Portable, and an OLAP showcase |
| 5 | Backoff style | Plain "stupid backoff": longest matching order, no probability discounting | Simple to implement and explain for a course project |

---

## 3. Architecture

```
                ┌──────────────────────────── C driver (thin) ────────────────────────────┐
                │  REPL loop  ·  arg parsing  ·  generation control loop  ·  backoff steps  │
                └───────────────────────────────────┬───────────────────────────────────────┘
                                                     │ DuckDB C API (prepared statements)
                                                     ▼
   ┌──────────────────────────────────────── DuckDB (markov.db) ───────────────────────────────────────┐
   │  read_text()+regex  →  tokens  →  window n-grams  →  ON CONFLICT merge  →  transitions             │
   │  vocabulary · documents · training_runs · transitions · generation_runs · generation_steps         │
   │  OLAP stat queries (RANK / ROLLUP / window entropy / perplexity)                                   │
   └────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

The C side never parses text and never holds the model. It (a) hands DuckDB a file path + order for
training, and (b) drives the per-step backoff loop for generation, logging each step.

---

## 4. Prerequisites & tooling setup

DuckDB is **not** installed system-wide; we vendor the C library locally (gitignored).

```bash
cd ~/Desktop/dev/systems/markov
mkdir -p libs build

# --- DuckDB C library (header + dylib), v1.5.3, universal macOS ---
VER=1.5.3
curl -L -o /tmp/libduckdb.zip \
  "https://github.com/duckdb/duckdb/releases/download/v${VER}/libduckdb-osx-universal.zip"
unzip -o /tmp/libduckdb.zip -d libs/macos      # -> libs/macos/{duckdb.h, duckdb.hpp, libduckdb.dylib}
# (on Linux: the libduckdb-linux-amd64.zip into libs/linux — the Makefile picks per-OS)
curl -L -o /tmp/libduckdb.zip \
  "https://github.com/duckdb/duckdb/releases/download/v${VER}/libduckdb-linux-amd64.zip"
unzip -o /tmp/libduckdb.zip -d libs/linux      # -> libs/macos/{duckdb.h, duckdb.hpp, libduckdb.dylib}

# --- DuckDB CLI (handy for verifying SQL by hand) ---
curl -L -o /tmp/duckdb-cli.zip \
  "https://github.com/duckdb/duckdb/releases/download/v${VER}/duckdb_cli-osx-universal.zip"
unzip -o /tmp/duckdb-cli.zip -d libs           # -> libs/duckdb (CLI binary)

# --- DuckDB CLI LINUX (handy for verifying SQL by hand) ---
curl -L -o /tmp/duckdb-cli.zip \
  "https://github.com/duckdb/duckdb/releases/download/v${VER}/duckdb_cli-linux-amd64.zip"
unzip -o /tmp/duckdb-cli.zip -d libs           # -> libs/duckdb (CLI binary)
```

Add the vendored binaries and the database to `.gitignore`:

```
build/
libs/
data/db/
temp/
*.dSYM/
```

Sanity-check the CLI and the regex tokenizer before writing any C:

```bash
./libs/duckdb -c "SELECT regexp_extract_all(lower('Hello, world! Don''t panic.'), '[\p{L}][\p{L}'']*');"
# -> [hello, world, don't, panic]
```

---

## 5. Repository layout (target)

```
markov/
├── README.md            ← this guide
├── Makefile             ← build rules (§7 step 0)
├── schema.sql           ← all CREATE TABLE / SEQUENCE statements (§6.1)
├── train_stage.sql      ← tokenize + vocabulary upsert + documents row (§7.2)
├── train_order.sql      ← per-order n-gram merge into transitions (§7.2)
├── markov.c             ← the whole driver: REPL, training, generation, stats (~345 lines)
├── data/
│   ├── long.txt         ← training corpus
│   └── db/markov.db     ← the database (gitignored)
└── libs/                ← vendored DuckDB: macos/, linux/, CLI binary (gitignored)
```

The old prototype files (`main.c`, `gen.c`, `db.txt`, `output.txt`) are retired; the corpus moved
to `data/long.txt`. There is no separate `db.h`/`db.c` — at this size the DB helpers live in
`markov.c` (see §7 step 1).

---

## 6. The database schema

Create `schema.sql`. DuckDB has no `AUTOINCREMENT`; use **sequences** for surrogate keys.

### 6.1 `schema.sql`

```sql
-- ---------- sequences for surrogate ids ----------
CREATE SEQUENCE IF NOT EXISTS seq_vocab   START 1;
CREATE SEQUENCE IF NOT EXISTS seq_doc     START 1;
CREATE SEQUENCE IF NOT EXISTS seq_trainrun START 1;
CREATE SEQUENCE IF NOT EXISTS seq_genrun  START 1;

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
    ord       TINYINT NOT NULL,                       -- context length
    state_key VARCHAR NOT NULL,                       -- space-joined previous `ord` tokens
    next_id   INTEGER NOT NULL REFERENCES vocabulary(id),
    count     BIGINT  NOT NULL DEFAULT 0,
    PRIMARY KEY (ord, state_key, next_id)             -- also the ON CONFLICT target
);

-- ---------- generation_runs: one row per :gen invocation ----------
CREATE TABLE IF NOT EXISTS generation_runs (
    id              INTEGER PRIMARY KEY DEFAULT nextval('seq_genrun'),
    seed            VARCHAR,
    requested_order TINYINT,
    max_tokens      INTEGER,
    created_at      TIMESTAMP DEFAULT now()
);

-- ---------- generation_steps: one row per generated token (the analytics goldmine) ----------
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
```

That's **6 relations** with foreign keys — above the 3–5 the assignment asks for, and every stat
query in §8 joins at least two of them.

> **Verified on DuckDB 1.5.3:** the `ON CONFLICT` upsert on `vocabulary` works even when the row
> is already referenced by `transitions.next_id`. Older DuckDB versions rejected updates to
> FK-referenced rows (updates are internally delete + insert), so re-verify this in the CLI if you
> pin a different version — it is the one schema decision that depends on it.

---

## 7. Implementation, step by step

### Step 0 — Makefile

```makefile
# Detect host OS to select the right vendored DuckDB library
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	LIBDIR := $(CURDIR)/libs/macos
else
	LIBDIR := $(CURDIR)/libs/linux
endif

CFLAGS := -Wall -Wextra -O2 -I$(LIBDIR)
LDFLAGS := -L$(LIBDIR) -lduckdb -Wl,-rpath,$(LIBDIR)
BIN := build/markov

all: $(BIN)

$(BIN): markov.c
	mkdir -p build
	gcc $(CFLAGS) -o $(BIN) markov.c $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -rf build
```

The `$(CURDIR)`-absolute rpath means the binary finds `libduckdb` no matter where it is launched
from (a relative `-rpath,libs` would break outside the repo root).

### Step 1 — `db.h` / `db.c`: connection + schema + helpers

`db.h`:

```c
#ifndef DB_H
#define DB_H
#include "duckdb.h"

typedef struct {
    duckdb_database   db;
    duckdb_connection con;
} DB;

int  db_open(DB *d, const char *path);     // open + connect + run schema.sql
void db_close(DB *d);
int  db_exec(DB *d, const char *sql);      // run a statement, print errors, return 0/1
#endif
```

`db.c` (essentials):

```c
#include "db.h"
#include <stdio.h>
#include <stdlib.h>

static char *slurp(const char *path) {            // read schema.sql into a string
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *buf = malloc(n + 1);
    fread(buf, 1, n, f); buf[n] = '\0'; fclose(f);
    return buf;
}

int db_exec(DB *d, const char *sql) {
    duckdb_result r;
    if (duckdb_query(d->con, sql, &r) == DuckDBError) {
        fprintf(stderr, "[SQL ERROR] %s\n", duckdb_result_error(&r));
        duckdb_destroy_result(&r);
        return 1;
    }
    duckdb_destroy_result(&r);
    return 0;
}

int db_open(DB *d, const char *path) {
    if (duckdb_open(path, &d->db) == DuckDBError) return 1;
    if (duckdb_connect(d->db, &d->con) == DuckDBError) return 1;
    char *schema = slurp("schema.sql");
    if (!schema) { fprintf(stderr, "cannot read schema.sql\n"); return 1; }
    int rc = db_exec(d, schema);
    free(schema);
    return rc;
}

void db_close(DB *d) {
    duckdb_disconnect(&d->con);
    duckdb_close(&d->db);
}
```

> **C API cheatsheet.** `duckdb_query` (ad-hoc SQL) · `duckdb_prepare` / `duckdb_bind_varchar` /
> `duckdb_bind_int64` / `duckdb_execute_prepared` (parameterized) · result access via
> `duckdb_row_count`, `duckdb_value_varchar`, `duckdb_value_int64`, `duckdb_value_double`
> (remember to `duckdb_free` strings from `duckdb_value_varchar`).

### Step 2 — Training (pure SQL, driven by C)

Training is **one SQL script** parameterized by the file path and the order `k`. The C side just
substitutes those two values and runs it. Build it as a prepared/`printf`'d script:

```sql
-- 2a. Stage the document as positioned, sentence-segmented, lowercased tokens.
--     Sentinels <s>/</s> wrap each sentence so generation can begin and end.
CREATE OR REPLACE TEMP TABLE doc_tokens AS
WITH raw AS (
    SELECT content FROM read_text('{{PATH}}')
),
sentences AS (                      -- split on . ! ? ; keep a stable sentence id
    SELECT s_id,
           list_concat(['<s>'],
                       regexp_extract_all(lower(sent), '[\p{L}][\p{L}'']*'),
                       ['</s>']) AS toks
    FROM (
        SELECT generate_subscripts(parts, 1) AS s_id, unnest(parts) AS sent
        FROM (SELECT string_split_regex(content, '[.!?]+') AS parts FROM raw)
    )
    WHERE len(regexp_extract_all(lower(sent), '[\p{L}][\p{L}'']*')) > 0
)
SELECT s_id,
       p.pos AS pos,
       toks[p.pos] AS token
FROM sentences
CROSS JOIN UNNEST(generate_series(1, len(toks))) AS p(pos);

-- THEN WE WANT TO DO, TO OBSERVE OUR TOKENIZED FILE
SELECT * FROM doc_tokens ORDER BY s_id, pos;


-- 2b. Upsert global vocabulary frequencies.
INSERT INTO vocabulary (token, freq)
SELECT token, COUNT(*) FROM doc_tokens GROUP BY token
ON CONFLICT (token) DO UPDATE SET freq = vocabulary.freq + excluded.freq;

-- 2c. Build order-k (state -> next) n-grams within sentence boundaries.
CREATE OR REPLACE TEMP TABLE ngrams AS
SELECT
    string_agg(token, ' ') OVER w  AS state_key,   -- k tokens: CURRENT .. (k-1) FOLLOWING
    lead(token, {{K}}) OVER (PARTITION BY s_id ORDER BY pos) AS next_token,
    COUNT(*)            OVER w  AS span             -- == k only when full window present
FROM doc_tokens
WINDOW w AS (PARTITION BY s_id ORDER BY pos
             ROWS BETWEEN CURRENT ROW AND {{K_MINUS_1}} FOLLOWING);

-- 2d. Merge into the model (one set-based upsert — DuckDB's happy path).
INSERT INTO transitions (ord, state_key, next_id, count)
SELECT {{K}}, g.state_key, v.id, COUNT(*)
FROM ngrams g
JOIN vocabulary v ON v.token = g.next_token
WHERE g.next_token IS NOT NULL AND g.span = {{K}}
GROUP BY g.state_key, v.id
ON CONFLICT (ord, state_key, next_id)
  DO UPDATE SET count = transitions.count + excluded.count;

-- 2e. Bookkeeping.
INSERT INTO documents (name, token_count)
SELECT '{{PATH}}', (SELECT COUNT(*) FROM doc_tokens);
INSERT INTO training_runs (document_id, ord, ngrams_added)
SELECT currval('seq_doc'), {{K}}, (SELECT COUNT(*) FROM ngrams WHERE next_token IS NOT NULL);
```

Replace `{{PATH}}`, `{{K}}`, `{{K_MINUS_1}}` in C (validate `K` is 1..9 to avoid injection; the path
comes from the local user so escaping single quotes is enough). **Verify 2a–2c in the CLI first**
with a tiny text file before wiring C.

### Step 3 — Generation with backoff + step logging

The control loop lives in C; the *sampling* is one SQL query. Prepare this once:

```sql
-- $1 = ord, $2 = state_key   → returns the sampled next token, its prob, and candidate count
WITH cand AS (
    SELECT next_id, count,
           SUM(count) OVER (ORDER BY next_id ROWS UNBOUNDED PRECEDING) AS cum,
           SUM(count) OVER ()                                          AS total
    FROM transitions
    WHERE ord = $1 AND state_key = $2
),
draw AS (SELECT random() * (SELECT MAX(total) FROM cand) AS x)
SELECT c.next_id,
       v.token,
       c.count::DOUBLE / c.total          AS prob,
       (SELECT COUNT(*) FROM cand)        AS n_candidates
FROM cand c, draw, vocabulary v
WHERE v.id = c.next_id AND c.cum >= draw.x
ORDER BY c.cum
LIMIT 1;   -- weighted reservoir: first cumulative bucket past the random draw
```

C generation loop (pseudocode):

```
create generation_runs row -> run_id
history = ["<s>"]               (or seed tokens, tokenized the same way)
for step in 0 .. max_tokens-1:
    for ord = min(requested_order, len(history)) down to 1:
        state_key = join(last `ord` tokens of history, " ")
        row = run sampling query (ord, state_key)
        if row exists: break          # longest matching order wins  (backoff)
    if no row at any order: stop       # dead end
    append row.token to history
    INSERT generation_steps(run_id, step, state_key, ord, row.next_id, row.prob, row.n_candidates)
    if row.token == "</s>": stop       # natural sentence end
print history without sentinels
```

Logging `order_used` (the `ord` that broke the loop) is what makes the analytics in §8 possible.

### Step 4 — The REPL

`markov.c` opens the DB (path from `argv[1]`, default `markov.db`) and loops on stdin:

```
:train <file> <order>     tokenize <file> and merge order-<order> n-grams into the model
:gen <order> [n] [seed…]  generate up to n tokens (default 50) at <order>, optional seed phrase
:top <order> <state…>     show the next-token distribution for a state (debug)
:stats <name>             run a named OLAP query from §8 (zipf, backoff, perplexity, entropy, growth)
:reset                    DROP + recreate schema
:help / :quit
```

Parse the first whitespace-delimited token as the command; dispatch with `strcmp`. Keep it under
~250 lines — all heavy lifting is SQL.

### Step 5 — Seed handling for `:gen`

A typed seed must be tokenized **the same way** as training (lowercase, same regex, no sentinels
unless you prepend `<s>`). Easiest: send the seed through a one-row SQL tokenization
(`regexp_extract_all(lower($1), '[\p{L}][\p{L}'']*')`) and read the list back, so training and
inference share identical tokenization. If the seed is empty, start from `["<s>"]`.

---

## 8. The OLAP statistics suite (`:stats <name>`)

These are the graded "5–10 SELECT queries." Each is self-contained; verify in the CLI.

```sql
-- (1) zipf : token rank vs frequency (RANK window) — expect a power law
SELECT token, freq, RANK() OVER (ORDER BY freq DESC) AS rank
FROM vocabulary ORDER BY rank LIMIT 30;

-- (2) backoff : how often each order actually fired, with a grand total (ROLLUP)
SELECT order_used, COUNT(*) AS steps,
       ROUND(100.0 * COUNT(*) / SUM(COUNT(*)) OVER (), 1) AS pct
FROM generation_steps
GROUP BY ROLLUP(order_used) ORDER BY order_used NULLS LAST;

-- (3) perplexity : per-run avg surprise and perplexity (2^entropy)
SELECT run_id,
       COUNT(*)                                   AS tokens,
       ROUND(-AVG(log2(chosen_prob)), 3)          AS avg_surprise_bits,
       ROUND(pow(2, -AVG(log2(chosen_prob))), 2)  AS perplexity
FROM generation_steps GROUP BY run_id ORDER BY run_id;

-- (4) entropy : most ambiguous states (highest branching entropy) at a given order
WITH p AS (
    SELECT ord, state_key, next_id,
           count::DOUBLE / SUM(count) OVER (PARTITION BY ord, state_key) AS prob
    FROM transitions
)
SELECT ord, state_key,
       COUNT(*)                       AS branches,
       ROUND(-SUM(prob*log2(prob)),3) AS entropy_bits
FROM p GROUP BY ord, state_key
HAVING COUNT(*) > 1
ORDER BY entropy_bits DESC LIMIT 20;

-- (5) deterministic : states with exactly one successor (entropy 0) — the model's "idioms"
SELECT ord, state_key, ANY_VALUE(v.token) AS always_next, ANY_VALUE(t.count) AS seen
FROM transitions t JOIN vocabulary v ON v.id = t.next_id
GROUP BY ord, state_key HAVING COUNT(*) = 1
ORDER BY seen DESC LIMIT 20;

-- (6) growth : cumulative vocabulary size across training runs (running window)
SELECT tr.id AS run, d.name, tr.ord, tr.ngrams_added,
       SUM(tr.ngrams_added) OVER (ORDER BY tr.id) AS cumulative_ngrams
FROM training_runs tr JOIN documents d ON d.id = tr.document_id
ORDER BY tr.id;

-- (7) contribution : distinct transitions and orders contributed per document
SELECT d.name, COUNT(DISTINCT tr.ord) AS orders_trained,
       SUM(tr.ngrams_added) AS ngrams
FROM documents d JOIN training_runs tr ON tr.document_id = d.id
GROUP BY d.name ORDER BY ngrams DESC;

-- (8) surprise leaderboard : tokens that were hardest to predict when generated
SELECT v.token, COUNT(*) AS times,
       ROUND(AVG(-log2(gs.chosen_prob)), 3) AS avg_surprise_bits
FROM generation_steps gs JOIN vocabulary v ON v.id = gs.chosen_next_id
GROUP BY v.token HAVING COUNT(*) >= 3
ORDER BY avg_surprise_bits DESC LIMIT 20;

-- (9) model size : transitions and states per order
SELECT ord, COUNT(*) AS transitions, COUNT(DISTINCT state_key) AS states
FROM transitions GROUP BY ord ORDER BY ord;

-- (10) coverage : share of high-order (>=2) hits vs order-1 fallbacks across all generation
SELECT CASE WHEN order_used >= 2 THEN 'high-order' ELSE 'order-1 fallback' END AS kind,
       COUNT(*) AS steps
FROM generation_steps GROUP BY 1;
```

---

## 9. Migrating from the old prototype

The current `main.c` keeps the model in fixed arrays (`keys[MAX_SIZE][100]`, `tokens[MAX_SIZE]`)
and tokenizes with a hand-rolled `fgetc` loop that has real bugs (`char ch = fgetc(...)` breaks EOF
detection; `isblank` ignores `\n`; digits/punctuation dropped; no bounds check on
`current_token[100]`; no lowercasing; ASCII-only `isalpha`). All of that is replaced by the SQL
tokenizer in §7.2. The only logic worth carrying over is the *idea* of `createKey` (joining `degree`
tokens into a state) — now expressed as `string_agg(... OVER w)`. Delete `main.c`, `gen.c`,
`db.txt`, `output.txt`; move `long.txt` into `corpora/`.

---

## 10. Build, run, and a smoke test

```bash
make                                  # builds build/markov, links vendored libduckdb
./build/markov markov.db              # opens the REPL on markov.db

# inside the REPL:
:train corpora/long.txt 1             # train order-1
:train corpora/long.txt 3             # add order-3 to the same model (mixed orders)
:gen 3 60                             # generate up to 60 tokens at order 3 (backs off to 1)
:stats backoff                        # see how often order 3 fired vs fell back
:stats perplexity                     # surprise/perplexity of your generations
:stats zipf                           # confirm the corpus follows Zipf's law
```

Expected: `:gen` produces text that begins after `<s>` and ends at `</s>`; `:stats backoff` shows
most steps firing at order 1 on a small corpus (sparse high-order contexts) — which is exactly the
phenomenon the backoff design exists to handle, and good material for the report.

---

## 11. Report checklist (maps to the rubric)

- **Quantity / complexity** — 6 relations, incremental + mixed-order training, backoff inference,
  SQL tokenization, 10 OLAP queries, interactive REPL.
- **Correctness** — verify each SQL block in the CLI before wiring C; the smoke test in §10 is the
  end-to-end check.
- **Originality** — a language model whose entire state and analytics live in an embedded OLAP DB;
  the DuckDB-vs-SQLite trade-off argued explicitly (§1, §2); Zipf/perplexity/entropy as
  database queries.
- **Technical-choice argument** — §1 (why DuckDB, batched writes), §2 (design table).

---

## 12. Stretch goals (only if time allows)

- **`:train` a whole folder** with `read_text('corpora/*.txt')` (DuckDB globs natively).
- **PIVOT** the backoff profile per run for a compact dashboard view.
- **Temperature** sampling: bias the §7.3 draw toward higher-count successors.
- **Export** generated runs to Parquet (`COPY ... TO 'out.parquet'`) to show DuckDB's analytics I/O.



make demo > demo_transcript.txt 2>&1
