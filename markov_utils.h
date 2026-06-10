/* markov_utils.h — stb-style single-header utility library.
 * Terminal colours, SI number formatting, wall-clock timing, file slurping,
 * string replacement and aligned table printing. No project dependencies.
 *
 * Usage: #include "markov_utils.h" anywhere; in exactly one translation
 * unit, #define MARKOV_UTILS_IMPLEMENTATION before the include. */
#ifndef MARKOV_UTILS_H
#define MARKOV_UTILS_H

#include <stddef.h>

/* program version — deploy.sh reads this to name the release package */
#define MARKOV_VERSION "1.1.0"

/* ANSI colours — empty strings until mu_colors_init() sees a terminal, so
 * piped transcripts (make demo > file) stay free of escape codes */
extern const char *BOLD, *DIM, *RED, *CYAN, *RESET;

void mu_colors_init(void);

/* 2.6M · 947.6K · 93 — buf must hold at least 32 bytes */
char *mu_fmt_si(double v, char *buf);

double mu_now_sec(void);

/* whole file as a malloc'd NUL-terminated string, NULL on error */
char *mu_slurp(const char *path);

/* malloc'd copy of src with every needle replaced by repl */
char *mu_str_replace(const char *src, const char *needle, const char *repl);

/* aligned table: cell holds (nr+1)*nc strings, row 0 is the header;
 * is_num[c] right-aligns column c; cells are not freed */
void mu_print_table(char **cell, size_t nc, size_t nr, const int *is_num);

#endif /* MARKOV_UTILS_H */

#ifdef MARKOV_UTILS_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

const char *BOLD = "", *DIM = "", *RED = "", *CYAN = "", *RESET = "";

void mu_colors_init(void) {
    if (!isatty(STDOUT_FILENO)) return;
    BOLD = "\033[1m"; DIM = "\033[2m"; RED = "\033[31m";
    CYAN = "\033[36m"; RESET = "\033[0m";
}

char *mu_fmt_si(double v, char *buf) {
    if (v >= 1e6)      sprintf(buf, "%.1fM", v / 1e6);
    else if (v >= 1e3) sprintf(buf, "%.1fK", v / 1e3);
    else               sprintf(buf, "%.0f", v);
    return buf;
}

double mu_now_sec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

char *mu_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *buf = malloc(n + 1);
    fread(buf, 1, n, f); buf[n] = '\0'; fclose(f);
    return buf;
}

char *mu_str_replace(const char *src, const char *needle, const char *repl) {
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

void mu_print_table(char **cell, size_t nc, size_t nr, const int *is_num) {
    if (nc == 0) return;
    size_t *w = calloc(nc, sizeof *w);
    for (size_t i = 0; i < nc * (nr + 1); i++)
        if (strlen(cell[i]) > w[i % nc]) w[i % nc] = strlen(cell[i]);
    for (size_t row = 0; row <= nr; row++) {
        for (size_t c = 0; c < nc; c++) {
            char *s = cell[row * nc + c];
            if (row == 0) printf(" %s%-*s%s ", BOLD, (int)w[c], s, RESET);
            else          printf(is_num[c] ? " %*s " : " %-*s ", (int)w[c], s);
            putchar(c + 1 < nc ? '|' : '\n');
        }
        if (row == 0) {                     /* header rule */
            printf("%s", DIM);
            for (size_t c = 0; c < nc; c++) {
                for (size_t i = 0; i < w[c] + 2; i++) putchar('-');
                putchar(c + 1 < nc ? '+' : '\n');
            }
            printf("%s", RESET);
        }
    }
    printf("%s (%zu row%s)%s\n", DIM, nr, nr == 1 ? "" : "s", RESET);
    free(w);
}

#endif /* MARKOV_UTILS_IMPLEMENTATION */
