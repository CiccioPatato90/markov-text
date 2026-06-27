# From an In-Memory Markov Prototype to a DuckDB-Backed Model

## Problem and scope

This project implements a word-level Markov model. Given an order `k`, it counts
which token follows each sequence of `k` tokens. At order 2, for example,
`the cat eats fish` contains these transitions:

```text
the cat  -> eats
cat eats -> fish
```

Generation starts with a seed or a sentence marker and repeatedly samples a
successor from the recorded counts. The model captures local patterns in its
training text; it does not model syntax or meaning.

There are two implementations in the repository. `old.c` is the initial,
in-memory prototype. `markov.c` moves the model and most training work into
DuckDB. The comparison below focuses on what that change buys and what it costs.

## The original prototype

`old.c` reads one hardcoded text file, tokenizes it in C, and stores the model in
global arrays:

```c
#define MAX_SIZE 2056

size_t size = 0;
char keys[MAX_SIZE][100];
Tokens tokens[MAX_SIZE];
```

This is enough to demonstrate the algorithm. It is also a hard capacity limit:
the number and length of states are fixed at compile time. The model disappears
when the process exits, and there is no record of its source data. Training a
second document means building another temporary model or modifying the program.

## What changed

The current implementation stores the model in six DuckDB tables. Its central
relation is `transitions`:

```sql
CREATE TABLE IF NOT EXISTS transitions (
    ord       TINYINT NOT NULL,
    state_key VARCHAR NOT NULL,
    next_id   INTEGER NOT NULL REFERENCES vocabulary(id),
    count     BIGINT  NOT NULL DEFAULT 0,
    PRIMARY KEY (ord, state_key, next_id)
);
```

`state_key` is the context, `next_id` identifies a token, and `count` is the
number of observations. Different orders share the table. The remaining tables
store vocabulary frequencies, source documents, training runs, generation runs,
and individual generation steps.

| Aspect | `old.c` | `markov.c` and DuckDB |
|---|---|---|
| Storage | Fixed C arrays in RAM | Persistent relational tables |
| Capacity | Compile-time constants | Disk, memory, and processing limits |
| Training | Rebuilt on each execution | Incremental merge into an existing model |
| Inputs | One hardcoded text file | Text, CSV, and Parquet |
| History | None | Documents and training runs |
| Inspection | Debug prints | SQL-backed statistics |
| Distribution | Source corpus plus program | Program plus an optional trained database |

### Incremental training

Training counts are merged rather than replaced:

```sql
ON CONFLICT (ord, state_key, next_id)
DO UPDATE SET count = transitions.count + excluded.count;
```

The default database is `data/db/markov.db`. Closing the program and reopening
that file preserves the model. The `documents` table also prevents accidental
double training of the same source; the `:train!` command is available when a
repeat is deliberate.

Training order `k` builds every order from 1 through `k`. This matters during
generation: if a long context is absent, the generator can back off to a shorter
one instead of stopping immediately.

### Input and SQL processing

Text files are read with DuckDB's `read_text`. CSV and Parquet inputs are read
through `read_csv` and `read_parquet`, with a selectable text column. SQL then
does token extraction, grouping, transition counting, vocabulary joins, and the
final upsert. C selects the input reader and runs the per-order statements.

This division is a good fit for the workload. A Markov model produces many
grouping and counting operations, while generation and the REPL need a modest
amount of procedural control. DuckDB would be less attractive for a design that
performed a separate transaction for every token; this implementation batches a
whole training source into set-based statements.

### Model inspection

Generation steps are logged with the order used, sampled token, probability,
and candidate count. The REPL exposes queries over this data:

```text
:stats zipf
:stats size
:stats growth
:stats entropy
:stats backoff
:stats perplexity
```

These are not estimates reconstructed from console output. They query the stored
model and generation history. For example, `backoff` shows how often generation
had to use a shorter context, and `size` reports states and transitions by order.

The model can also be pruned by transition count. Removing rare higher-order
rows reduces its size, while the normal backoff path still allows generation to
continue from lower orders.

## Local measurements

The following runs used the corpora currently in `data/`. They are point
measurements from the development machine, not a general DuckDB benchmark. The
old prototype was tested from a temporary copy with only its hardcoded paths
changed.

| Program | Input | Order | Result |
|---|---:|---:|---|
| `old.c` | `data/long.txt` | 1 | completed in about `0.01s` |
| `old.c` | `data/large_text.txt` | 1 | crashed with signal 11 |
| DuckDB version | `data/long.txt` | 1 | trained `1.3K` tokens; stored `1,096` transitions |
| DuckDB version | `data/large_text.txt` | 1 | trained `236K` tokens in `1.7s`; stored `235,978` transitions |
| DuckDB version | `data/books.txt` | 1 | trained `1.2M` tokens in `2.0s`; stored `334,313` transitions |
| DuckDB version | `data/large_text.txt` | 1–3 | trained three orders in `4.0s`; stored `707,933` transitions |

The small input favors the prototype: it has no database setup and little work
to do. The larger run reveals the more relevant difference. `old.c` writes past
the assumptions of its fixed-size representation and crashes, whereas the
DuckDB version completes the training and leaves a reusable model on disk.

The transition totals should not be read as memory-capacity claims. DuckDB is
still constrained by available storage and by the memory needed for its queries.
The useful distinction is that those limits are operational rather than encoded
as a 2,056-entry array in the program.

## Distribution

A trained DuckDB file contains the vocabulary, transition counts, source
metadata, training history, and generation logs. It can therefore be copied with
the executable as a pretrained model. The recipient can generate text or inspect
the model without obtaining the original corpus and repeating training.

There are practical caveats. A database copied while its write-ahead log still
contains pending work may be incomplete, and distributing generation logs may be
unwanted. `deploy.sh --with-db` checks for a non-empty WAL before packaging the
default model. A production packaging process would also decide whether to clear
history tables and verify DuckDB version compatibility.

## Costs and limitations

The current version has more moving parts: a schema, SQL templates, DuckDB
headers and libraries, persistent state, and failure cases around database I/O.
For teaching the Markov algorithm alone, `old.c` is shorter and easier to follow.

DuckDB also does not remove all scaling concerns. Higher-order models create many
mostly unique states; training can consume substantial memory, and the database
can become dominated by transitions observed once. The `:prune` command addresses
storage after training but does not reduce the peak cost of constructing those
counts.

The measurements above compare outcomes, not equivalent optimized programs. The
prototype's crash follows from its representation, but it could be rewritten
with dynamic hash tables and persistence without using DuckDB. The claim here is
therefore narrower: DuckDB provides those capabilities in this implementation
and makes the resulting model directly queryable.

## Conclusion

Moving the transition map into DuckDB changes the program's useful lifetime. A
training run now produces a durable model that can be extended, queried, pruned,
and distributed. C remains responsible for the REPL and generation loop, while
SQL handles the bulk transformations used to build and inspect the model.

That added machinery is unnecessary for a tiny one-shot generator. It is
justified here because persistence, larger corpora, mixed orders, and model
analysis are part of the project rather than optional additions.
