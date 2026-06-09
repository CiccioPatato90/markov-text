/* markov.c — DuckDB-backed multi-order Markov text engine.
 * The model lives entirely in the database; this file is a thin driver:
 * REPL, training driver (SQL templates), backoff generation loop, stats. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "duckdb.h"

#define MAX_ORDER   9
#define MAX_HISTORY 32768
#define BUF         8192

/* ANSI colours — enabled only when stdout is a terminal, so piped
 * transcripts (make demo > file) stay free of escape codes */
static const char *BOLD = "", *DIM = "", *RED = "", *CYAN = "", *RESET = "";

static void colors_init(void) {
    if (!isatty(STDOUT_FILENO)) return;
    BOLD = "\033[1m"; DIM = "\033[2m"; RED = "\033[31m";
    CYAN = "\033[36m"; RESET = "\033[0m";
}

/* ---------------- small utilities ---------------- */

static double now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *buf = malloc(n + 1);
    fread(buf, 1, n, f); buf[n] = '\0'; fclose(f);
    return buf;
}

static char *str_replace(const char *src, const char *needle, const char *repl) {
    size_t nlen = strlen(needle), rlen = strlen(repl), count = 0;
    for (const char *p = src; (p = strstr(p, needle)) != NULL; p += nlen) count++;
    char *out = malloc(strlen(src) + count * (rlen > nlen ? rlen - nlen : 0) + 1);
    char *o = out;
    const char *p = src, *q;
    while ((q = strstr(p, needle)) != NULL) {
        memcpy(o, p, (size_t)(q - p)); o += q - p;
        memcpy(o, repl, rlen);         o += rlen;
        p = q + nlen;
    }
    strcpy(o, p);
    return out;
}

/* ---------------- DB helpers ---------------- */

static int db_exec(duckdb_connection con, const char *sql) {
    duckdb_result r;
    if (duckdb_query(con, sql, &r) == DuckDBError) {
        fprintf(stderr, "%s[SQL ERROR]%s %s\n", RED, RESET, duckdb_result_error(&r));
        duckdb_destroy_result(&r);
        return 1;
    }
    duckdb_destroy_result(&r);
    return 0;
}

static int db_exec_file(duckdb_connection con, const char *path) {
    char *sql = slurp(path);
    if (!sql) return 1;
    int rc = db_exec(con, sql);
    free(sql);
    return rc;
}

static void print_result(duckdb_result *r) {
    idx_t nc = duckdb_column_count(r), nr = duckdb_row_count(r);
    if (nc == 0) return;
    /* materialize header + cells to compute column widths; numbers right-align */
    char  **cell = malloc(sizeof(char *) * nc * (nr + 1));
    size_t *w    = calloc(nc, sizeof *w);
    int    *num  = calloc(nc, sizeof *num);
    for (idx_t c = 0; c < nc; c++) {
        duckdb_type t = duckdb_column_type(r, c);
        num[c]  = (t >= DUCKDB_TYPE_TINYINT && t <= DUCKDB_TYPE_DOUBLE) ||
                  t == DUCKDB_TYPE_HUGEINT || t == DUCKDB_TYPE_DECIMAL;
        cell[c] = strdup(duckdb_column_name(r, c));
    }
    for (idx_t row = 0; row < nr; row++)
        for (idx_t c = 0; c < nc; c++) {
            char *v = duckdb_value_varchar(r, c, row);
            cell[(row + 1) * nc + c] = strdup(v ? v : "-");
            duckdb_free(v);
        }
    for (idx_t i = 0; i < nc * (nr + 1); i++)
        if (strlen(cell[i]) > w[i % nc]) w[i % nc] = strlen(cell[i]);
    for (idx_t row = 0; row <= nr; row++) {
        for (idx_t c = 0; c < nc; c++) {
            char *s = cell[row * nc + c];
            if (row == 0) printf(" %s%-*s%s ", BOLD, (int)w[c], s, RESET);
            else          printf(num[c] ? " %*s " : " %-*s ", (int)w[c], s);
            putchar(c + 1 < nc ? '|' : '\n');
            free(s);
        }
        if (row == 0) {                     /* header rule */
            printf("%s", DIM);
            for (idx_t c = 0; c < nc; c++) {
                for (size_t i = 0; i < w[c] + 2; i++) putchar('-');
                putchar(c + 1 < nc ? '+' : '\n');
            }
            printf("%s", RESET);
        }
    }
    printf("%s (%llu row%s)%s\n", DIM, (unsigned long long)nr, nr == 1 ? "" : "s", RESET);
    free(cell); free(w); free(num);
}

static void run_and_print(duckdb_connection con, const char *sql) {
    duckdb_result r;
    if (duckdb_query(con, sql, &r) == DuckDBError)
        fprintf(stderr, "[SQL ERROR] %s\n", duckdb_result_error(&r));
    else
        print_result(&r);
    duckdb_destroy_result(&r);
}

static long query_long(duckdb_connection con, const char *sql) {
    duckdb_result r;
    long v = -1;
    if (duckdb_query(con, sql, &r) != DuckDBError && duckdb_row_count(&r) > 0)
        v = (long)duckdb_value_int64(&r, 0, 0);
    duckdb_destroy_result(&r);
    return v;
}

/* ---------------- :train ---------------- */

static void cmd_train(duckdb_connection con, const char *path, int k) {
    if (k < 1 || k > MAX_ORDER) { fprintf(stderr, "order must be 1..%d\n", MAX_ORDER); return; }

    char *stage_t = slurp("train_stage.sql");
    char *order_t = slurp("train_order.sql");
    if (!stage_t || !order_t) { free(stage_t); free(order_t); return; }

    char *esc   = str_replace(path, "'", "''");
    char *stage = str_replace(stage_t, "{{PATH}}", esc);
    double t0   = now_sec();
    if (db_exec(con, stage) == 0) {
        /* train every order 1..k so backoff never hits an untrained order */
        for (int ord = 1; ord <= k; ord++) {
            char kbuf[8], kmbuf[8];
            snprintf(kbuf, sizeof kbuf, "%d", ord);
            snprintf(kmbuf, sizeof kmbuf, "%d", ord - 1);
            char *s1 = str_replace(order_t, "{{K}}", kbuf);
            char *s2 = str_replace(s1, "{{KM1}}", kmbuf);
            if (db_exec(con, s2)) { free(s1); free(s2); break; }
            free(s1); free(s2);
        }
        printf("trained %s — %ld tokens staged in %.2fs, n-grams merged per order:\n",
               path, query_long(con, "SELECT COUNT(*) FROM doc_tokens"),
               now_sec() - t0);
        run_and_print(con,
            "SELECT ord AS \"order\", ngrams_added FROM training_runs"
            " WHERE document_id = currval('seq_doc') ORDER BY ord");
    }
    free(esc); free(stage); free(stage_t); free(order_t);
}

/* ---------------- :gen ---------------- */

static const char *SAMPLE_SQL =
    "SELECT t.next_id, v.token,"
    "       t.count::DOUBLE / SUM(t.count) OVER () AS prob,"
    "       COUNT(*) OVER () AS n_candidates "
    "FROM transitions t JOIN vocabulary v ON v.id = t.next_id "
    "WHERE t.ord = $1 AND t.state_key = $2 "
    "ORDER BY random() ^ (1.0 / t.count) DESC LIMIT 1";  /* weighted reservoir */

static const char *LOG_SQL =
    "INSERT INTO generation_steps (run_id, step, state_key, order_used,"
    " chosen_next_id, chosen_prob, n_candidates) VALUES ($1,$2,$3,$4,$5,$6,$7)";

static void cmd_gen(duckdb_connection con, int k, long max_tokens, const char *seed) {
    if (k < 1 || k > MAX_ORDER) { fprintf(stderr, "order must be 1..%d\n", MAX_ORDER); return; }
    if (max_tokens < 1) max_tokens = 50;
    if (max_tokens > MAX_HISTORY - 64) {   /* every step is a SELECT + INSERT */
        max_tokens = MAX_HISTORY - 64;
        printf("%s(capped at %ld tokens)%s\n", DIM, max_tokens, RESET);
    }

    char *hist[MAX_HISTORY];
    int   hlen = 0;

    /* seed goes through the SAME tokenization as training */
    if (seed && *seed) {
        duckdb_prepared_statement ps;
        duckdb_result r;
        duckdb_prepare(con,
            "SELECT unnest(regexp_extract_all(replace(lower($1), '’', ''''),"
            " '[\\p{L}][\\p{L}'']*'))", &ps);
        duckdb_bind_varchar(ps, 1, seed);
        if (duckdb_execute_prepared(ps, &r) != DuckDBError)
            for (idx_t i = 0; i < duckdb_row_count(&r) && hlen < MAX_ORDER * 4; i++) {
                char *t = duckdb_value_varchar(&r, 0, i);
                hist[hlen++] = strdup(t);
                duckdb_free(t);
            }
        duckdb_destroy_result(&r);
        duckdb_destroy_prepare(&ps);
    }
    if (hlen == 0) hist[hlen++] = strdup("<s>");

    /* bookkeeping row + its id */
    {
        duckdb_prepared_statement ps;
        duckdb_prepare(con,
            "INSERT INTO generation_runs (seed, requested_order, max_tokens)"
            " VALUES ($1,$2,$3)", &ps);
        duckdb_bind_varchar(ps, 1, seed ? seed : "");
        duckdb_bind_int32(ps, 2, k);
        duckdb_bind_int32(ps, 3, (int)max_tokens);
        duckdb_result r;
        duckdb_execute_prepared(ps, &r);
        duckdb_destroy_result(&r);
        duckdb_destroy_prepare(&ps);
    }
    long run_id = query_long(con, "SELECT currval('seq_genrun')");

    duckdb_prepared_statement sample, logstep;
    duckdb_prepare(con, SAMPLE_SQL, &sample);
    duckdb_prepare(con, LOG_SQL, &logstep);

    int tokens = 0;
    const char *why = "token limit";
    double t0 = now_sec();
    for (long step = 0; step < max_tokens; step++) {
        char state[BUF];
        char *token = NULL;
        long next_id = 0, n_cand = 0;
        double prob = 0;
        int used = 0;

        /* backoff: longest context we have data for wins */
        for (int ord = (hlen < k ? hlen : k); ord >= 1; ord--) {
            state[0] = '\0';
            for (int i = hlen - ord; i < hlen; i++) {
                if (i > hlen - ord) strlcat(state, " ", sizeof state);
                strlcat(state, hist[i], sizeof state);
            }
            duckdb_bind_int32(sample, 1, ord);
            duckdb_bind_varchar(sample, 2, state);
            duckdb_result r;
            if (duckdb_execute_prepared(sample, &r) != DuckDBError &&
                duckdb_row_count(&r) == 1) {
                next_id = (long)duckdb_value_int64(&r, 0, 0);
                char *t = duckdb_value_varchar(&r, 1, 0);
                token   = strdup(t);
                duckdb_free(t);
                prob    = duckdb_value_double(&r, 2, 0);
                n_cand  = (long)duckdb_value_int64(&r, 3, 0);
                used    = ord;
                duckdb_destroy_result(&r);
                break;
            }
            duckdb_destroy_result(&r);
        }
        if (!token) { why = "dead end (state unseen at every order)"; break; }

        duckdb_bind_int64(logstep, 1, run_id);
        duckdb_bind_int32(logstep, 2, (int)step);
        duckdb_bind_varchar(logstep, 3, state);
        duckdb_bind_int32(logstep, 4, used);
        duckdb_bind_int64(logstep, 5, next_id);
        duckdb_bind_double(logstep, 6, prob);
        duckdb_bind_int32(logstep, 7, (int)n_cand);
        duckdb_result lr;
        duckdb_execute_prepared(logstep, &lr);
        duckdb_destroy_result(&lr);

        hist[hlen++] = token;
        if (strcmp(token, "</s>") == 0) {
            /* sentence boundary, not a stop: open a fresh <s> context and go on */
            if (hlen >= MAX_HISTORY - 1) { why = "history limit"; break; }
            hist[hlen++] = strdup("<s>");
            continue;
        }
        tokens++;
        if (hlen >= MAX_HISTORY) { why = "history limit"; break; }
    }
    double dt = now_sec() - t0;

    duckdb_destroy_prepare(&sample);
    duckdb_destroy_prepare(&logstep);

    /* long outputs go to a file keyed by run id instead of flooding the console */
    FILE *out = stdout;
    char outpath[64] = "";
    if (tokens > 500) {
        mkdir("output", 0755);
        snprintf(outpath, sizeof outpath, "output/run_%ld.txt", run_id);
        out = fopen(outpath, "w");
        if (!out) {
            fprintf(stderr, "%scannot write %s%s\n", RED, outpath, RESET);
            out = stdout; outpath[0] = '\0';
        }
    }
    int printed = 0;
    for (int i = 0; i < hlen; i++) {
        if (!strcmp(hist[i], "</s>")) { if (printed) fputc('.', out); }
        else if (strcmp(hist[i], "<s>") != 0)
            fprintf(out, "%s%s", printed++ ? " " : "", hist[i]);
        free(hist[i]);
    }
    if (printed) fputc('\n', out);
    if (out != stdout) {
        fclose(out);
        printf("output is %d tokens — full text written to %s%s%s\n",
               tokens, BOLD, outpath, RESET);
    }
    printf("%s └─ run %ld · order %d · %d token%s · %.2fs · %.0f tok/s · stopped at %s%s\n",
           DIM, run_id, k, tokens, tokens == 1 ? "" : "s", dt,
           dt > 0 ? tokens / dt : 0.0, why, RESET);
}

/* ---------------- :stats ---------------- */

static const struct { const char *name; const char *sql; } STATS[] = {
    { "zipf",
      "SELECT token, freq, RANK() OVER (ORDER BY freq DESC) AS rank "
      "FROM vocabulary WHERE token NOT IN ('<s>','</s>') ORDER BY rank LIMIT 30" },
    { "backoff",
      "SELECT order_used, COUNT(*) AS steps,"
      " ROUND(100.0*COUNT(*)/(SELECT COUNT(*) FROM generation_steps),1) AS pct "
      "FROM generation_steps GROUP BY ROLLUP(order_used) ORDER BY order_used NULLS LAST" },
    { "perplexity",
      "SELECT run_id, COUNT(*) AS tokens,"
      " ROUND(-AVG(log2(chosen_prob)),3) AS avg_surprise_bits,"
      " ROUND(pow(2,-AVG(log2(chosen_prob))),2) AS perplexity "
      "FROM generation_steps GROUP BY run_id ORDER BY run_id" },
    { "entropy",
      "WITH p AS (SELECT ord, state_key,"
      "  count::DOUBLE/SUM(count) OVER (PARTITION BY ord,state_key) AS prob"
      "  FROM transitions) "
      "SELECT ord, state_key, COUNT(*) AS branches,"
      " ROUND(-SUM(prob*log2(prob)),3) AS entropy_bits "
      "FROM p GROUP BY ord, state_key HAVING COUNT(*) > 1 "
      "ORDER BY entropy_bits DESC LIMIT 20" },
    { "size",
      "SELECT ord, COUNT(*) AS transitions, COUNT(DISTINCT state_key) AS states "
      "FROM transitions GROUP BY ord ORDER BY ord" },
    { "growth",
      "SELECT tr.id AS run, d.name, tr.ord, tr.ngrams_added,"
      " SUM(tr.ngrams_added) OVER (ORDER BY tr.id) AS cumulative_ngrams "
      "FROM training_runs tr JOIN documents d ON d.id = tr.document_id ORDER BY tr.id" },
};
#define N_STATS (sizeof STATS / sizeof *STATS)

static void cmd_stats(duckdb_connection con, const char *name) {
    for (size_t i = 0; i < N_STATS; i++)
        if (strcmp(name, STATS[i].name) == 0) { run_and_print(con, STATS[i].sql); return; }
    fprintf(stderr, "%sunknown stat '%s'%s; available:", RED, name, RESET);
    for (size_t i = 0; i < N_STATS; i++) fprintf(stderr, " %s", STATS[i].name);
    fprintf(stderr, "\n");
}

/* ---------------- :reset / :help / REPL ---------------- */

static const char *RESET_SQL =  /* children first (FK dependency order) */
    "DROP TABLE IF EXISTS generation_steps; DROP TABLE IF EXISTS generation_runs;"
    "DROP TABLE IF EXISTS training_runs; DROP TABLE IF EXISTS transitions;"
    "DROP TABLE IF EXISTS documents; DROP TABLE IF EXISTS vocabulary;"
    "DROP SEQUENCE IF EXISTS seq_vocab; DROP SEQUENCE IF EXISTS seq_doc;"
    "DROP SEQUENCE IF EXISTS seq_trainrun; DROP SEQUENCE IF EXISTS seq_genrun;";

static void help(void) {
    printf("%s:train <file> <order>%s     tokenize <file>, merge n-grams at every order 1..<order>\n"
           "%s:gen <order> [n] [seed…]%s  generate up to n tokens (default 50), optional seed phrase\n"
           "%s:stats <name>%s             zipf | backoff | perplexity | entropy | size | growth\n"
           "%s:reset%s                    drop and recreate the schema\n"
           "%s:help / :quit%s\n",
           BOLD, RESET, BOLD, RESET, BOLD, RESET, BOLD, RESET, BOLD, RESET);
}

int main(int argc, char **argv) {
    const char *db_path = argc > 1 ? argv[1] : "data/db/markov.db";
    duckdb_database   db;
    duckdb_connection con;

    if (duckdb_open(db_path, &db) == DuckDBError) {
        fprintf(stderr, "failed to open %s\n", db_path); return EXIT_FAILURE;
    }
    if (duckdb_connect(db, &con) == DuckDBError) {
        fprintf(stderr, "failed to connect\n"); duckdb_close(&db); return EXIT_FAILURE;
    }
    if (db_exec_file(con, "schema.sql")) return EXIT_FAILURE;

    colors_init();
    printf("%smarkov-db%s — model in %s. :help for commands.\n", BOLD, RESET, db_path);
    int interactive = isatty(STDIN_FILENO);
    char line[BUF];
    while (printf("%smarkov>%s ", CYAN, RESET), fflush(stdout),
           fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = '\0';
        /* scripted runs (make demo): echo commands, treat # lines as narration */
        if (!interactive)
            printf("%s%s%s\n", line[0] == '#' ? DIM : BOLD, line, RESET);
        if (line[0] == '#') continue;
        char path[1024], name[64], seed[BUF];
        int k;
        long n;
        if (sscanf(line, ":train %1023s %d", path, &k) == 2) {
            cmd_train(con, path, k);
        } else if (!strncmp(line, ":gen", 4)) {
            n = 50; seed[0] = '\0';
            if (sscanf(line, ":gen %d %ld %[^\n]", &k, &n, seed) >= 2 ||
                sscanf(line, ":gen %d %[^\n]", &k, seed) >= 1)
                cmd_gen(con, k, n, seed);
            else
                fprintf(stderr, "usage: :gen <order> [n] [seed…]\n");
        } else if (!strncmp(line, ":stats", 6)) {
            if (sscanf(line, ":stats %63s", name) == 1) {
                cmd_stats(con, name);
            } else {
                printf("usage: :stats <name> — available:");
                for (size_t i = 0; i < N_STATS; i++) printf(" %s", STATS[i].name);
                printf("\n");
            }
        } else if (!strcmp(line, ":reset")) {
            if (!db_exec(con, RESET_SQL) && !db_exec_file(con, "schema.sql"))
                printf("schema reset.\n");
        } else if (!strcmp(line, ":help")) {
            help();
        } else if (!strcmp(line, ":quit") || !strcmp(line, ":q")) {
            break;
        } else if (line[0]) {
            fprintf(stderr, "%sunknown command%s — :help lists them\n", RED, RESET);
        }
    }

    duckdb_disconnect(&con);
    duckdb_close(&db);
    return EXIT_SUCCESS;
}
