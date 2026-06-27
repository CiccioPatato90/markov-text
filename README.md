# Markov-DB

Markov-DB is a word-level Markov text generator written in C. DuckDB stores the
vocabulary, transition counts, training history, and generation logs. Training is
incremental: the database remains on disk and can be extended in later sessions.

The project was built for the Université de Toulouse M2 CSA Embedded Databases
course (2025–2026).

## Run the demo

The repository includes a scripted demonstration. From the repository root, run:

```sh
make demo
```

This builds `build/markov`, creates a fresh `data/db/demo.db`, trains it with the
included `data/long.txt` and `data/short.txt` corpora, generates several samples,
and prints model statistics. The script intentionally deletes only the demo
database before each run; it does not touch the default database.

The final demo command requests 800 tokens, so its output is written to
`output/` instead of flooding the terminal. The commands used by the demo are in
[`demo.txt`](demo.txt).

To save a transcript:

```sh
make demo > demo-transcript.txt 2>&1
```

## Build requirements

On Windows, run the project inside WSL. Native Windows builds are not supported
by the current Makefile.

- GNU Make
- a C compiler (`gcc` by default)
- DuckDB 1.5.3 C headers and library

The Makefile expects the DuckDB files at these paths:

```text
libs/linux/duckdb.h
libs/linux/libduckdb.so
```

or, on macOS:

```text
libs/macos/duckdb.h
libs/macos/libduckdb.dylib
```

`libs/` is ignored by Git. Download the matching C API bundle from the
[DuckDB 1.5.3 release](https://github.com/duckdb/duckdb/releases/tag/v1.5.3)
and extract it into the appropriate directory. The DuckDB command-line client
is optional.

Build without running the demo:

```sh
make
```

The executable is `build/markov`. Its runtime library path is relative to the
executable, so no `LD_LIBRARY_PATH` or `DYLD_LIBRARY_PATH` setup is needed when
the library is in the location above.

## Interactive use

Start the REPL with its default persistent database:

```sh
./build/markov
```

Or choose another database file:

```sh
./build/markov my-model.db
```

A minimal session looks like this:

```text
:train data/short.txt 3
:gen 3 60
:gen 3 40 the city
:stats size
:quit
```

`:train data/short.txt 3` trains orders 1, 2, and 3, which gives generation a
complete backoff path. The database rejects the same source a second time to
prevent accidental double-counting; use `:train!` when repeating it is
intentional.

Available commands:

| Command | Purpose |
|---|---|
| `:train <file> <order> [column]` | Train from text, CSV, or Parquet |
| `:train! <file> <order> [column]` | Train even if the source was used before |
| `:gen <order> [tokens] [seed]` | Generate text with optional seed words |
| `:prune <min-count>` | Remove transitions seen fewer than this count |
| `:stats <name>` | Run `zipf`, `backoff`, `perplexity`, `entropy`, `size`, or `growth` |
| `:reset` | Delete the current model and recreate its schema |
| `:help` | Show command help |
| `:quit` | Exit |

For CSV and Parquet input, the optional column defaults to `text`:

```text
:train data/wikitext103.parquet 3 text
```

Paths are resolved from the directory where the program is started. Generated
text longer than 500 tokens is saved under `output/`.

## How it works

Training is split between C and SQL. `markov.c` handles the REPL and prepares
the input query. `train_stage.sql` tokenizes the source, updates the vocabulary,
and records the document. `train_order.sql` counts state-to-token transitions
for each requested order and merges them into DuckDB.

During generation, the program first looks for a transition at the requested
order. If the current state was not observed, it tries progressively shorter
states. The next token is sampled according to its observed count. Each choice
is stored in `generation_steps`, which is what the statistics commands query.

The schema is in [`schema.sql`](schema.sql). The report
[`docs/markov_duckdb_comparison_report.md`](docs/markov_duckdb_comparison_report.md)
compares this implementation with the original in-memory prototype in `old.c`.

## Repository map

```text
markov.c                 REPL, training orchestration, generation, statistics
markov_utils.h           small terminal and formatting helpers
schema.sql               persistent database schema
train_stage.sql          input reading and token staging
train_order.sql          n-gram aggregation and transition upsert
demo.txt                 commands executed by `make demo`
old.c                    original in-memory prototype
data/                    sample corpora; generated databases are ignored
docs/                    project report and supporting notes
```

Use `make clean` to remove the compiled binary and generated SQL header. Model
databases and generated output are not removed.
