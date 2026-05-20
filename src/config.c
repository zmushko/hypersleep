/*
 * config.c — load and validate /etc/hypersleep/hypersleep.conf.
 *
 * The format is line-oriented (docs/config.md):
 *
 *   - '#' starts a comment that runs to end-of-line.
 *   - blank lines are ignored.
 *   - a backslash at end-of-line continues onto the next line.
 *   - directives are case-insensitive; their first token is the
 *     directive name, the remainder are positional or key=value
 *     arguments.
 *   - the watch directive takes a positional path followed by
 *     key=value options:
 *        watch /home/u/projects exclude="\.git/" recursive=yes
 *   - quoted values may contain spaces and use backslash to escape
 *     the closing quote.
 *
 * Errors are reported via log_error with the file path and line
 * number; the parser keeps going so the operator sees every mistake
 * in one pass. config_load returns NULL if any error was reported.
 *
 * config_validate runs orthogonal semantic checks (paths exist,
 * etc.) and is meant to be called separately from `hypersleep config
 * test` and from hypersleepd startup.
 */

#include "config.h"
#include "log.h"
#include "timeparse.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_TOKENS 64

/* ------------------------------------------------------------------ */
/* tiny helpers                                                       */
/* ------------------------------------------------------------------ */

static char *xstrdup(const char *s)
{
    char *r = strdup(s);
    if (r == NULL) {
        log_error("out of memory");
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* defaults                                                           */
/* ------------------------------------------------------------------ */

static int apply_defaults(hs_config_t *cfg)
{
    cfg->store_path     = xstrdup("/var/lib/hypersleep/store");
    cfg->index_path     = xstrdup("/var/lib/hypersleep/index");
    cfg->log_path       = NULL;
    cfg->control_socket = xstrdup("/run/hypersleep/control.sock");
    cfg->lock_path      = xstrdup("/var/lib/hypersleep/lock");
    cfg->log_level      = HS_LOG_INFO;
    cfg->inotify_max_watches_warn = 524288;
    cfg->debounce_ms    = 1500;
    cfg->queue_poll_ms  = 500;
    cfg->compress_default  = false;
    cfg->compress_min_size = 4096;
    cfg->fsync_mode     = FSYNC_FULL;
    cfg->store_fmode    = 0600;
    cfg->retention_default_ns = 0;
    cfg->gc_after_prune = true;

    if (cfg->store_path == NULL || cfg->index_path == NULL
        || cfg->control_socket == NULL || cfg->lock_path == NULL) {
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* token-level parsers                                                */
/* ------------------------------------------------------------------ */

static int parse_uint(const char *val, unsigned *out,
                      const char *what, int lineno)
{
    char *end = NULL;
    errno = 0;
    unsigned long n = strtoul(val, &end, 0);
    if (errno != 0 || end == val || *end != '\0' || n > UINT_MAX) {
        log_error("line %d: %s expects unsigned integer, got '%s'",
                  lineno, what, val);
        return -1;
    }
    *out = (unsigned)n;
    return 0;
}

static int parse_size(const char *val, size_t *out,
                      const char *what, int lineno)
{
    char *end = NULL;
    errno = 0;
    unsigned long long n = strtoull(val, &end, 0);
    if (errno != 0 || end == val || *end != '\0' || n > SIZE_MAX) {
        log_error("line %d: %s expects size in bytes, got '%s'",
                  lineno, what, val);
        return -1;
    }
    *out = (size_t)n;
    return 0;
}

static int parse_octal_mode(const char *val, unsigned *out,
                            const char *what, int lineno)
{
    char *end = NULL;
    errno = 0;
    unsigned long n = strtoul(val, &end, 8);
    if (errno != 0 || end == val || *end != '\0' || n > 07777) {
        log_error("line %d: %s expects octal mode (e.g. 0600), got '%s'",
                  lineno, what, val);
        return -1;
    }
    *out = (unsigned)n;
    return 0;
}

static int parse_bool(const char *val, bool *out,
                      const char *what, int lineno)
{
    if (!strcasecmp(val, "yes")  || !strcasecmp(val, "true")
        || !strcasecmp(val, "on") || !strcmp(val, "1")) {
        *out = true;
        return 0;
    }
    if (!strcasecmp(val, "no")  || !strcasecmp(val, "false")
        || !strcasecmp(val, "off") || !strcmp(val, "0")) {
        *out = false;
        return 0;
    }
    log_error("line %d: %s expects yes/no, got '%s'", lineno, what, val);
    return -1;
}

static int parse_log_level(const char *val, enum hs_log_level *out, int lineno)
{
    if (!strcasecmp(val, "debug")) { *out = HS_LOG_DEBUG; return 0; }
    if (!strcasecmp(val, "info"))  { *out = HS_LOG_INFO;  return 0; }
    if (!strcasecmp(val, "warn") || !strcasecmp(val, "warning")) {
        *out = HS_LOG_WARN;
        return 0;
    }
    if (!strcasecmp(val, "error") || !strcasecmp(val, "err")) {
        *out = HS_LOG_ERROR;
        return 0;
    }
    log_error("line %d: log-level must be debug|info|warn|error, got '%s'",
              lineno, val);
    return -1;
}

static int parse_fsync_mode(const char *val, int *out, int lineno)
{
    if (!strcasecmp(val, "full")) { *out = FSYNC_FULL; return 0; }
    if (!strcasecmp(val, "data")) { *out = FSYNC_DATA; return 0; }
    if (!strcasecmp(val, "none")) { *out = FSYNC_NONE; return 0; }
    log_error("line %d: fsync-mode must be full|data|none, got '%s'",
              lineno, val);
    return -1;
}

static int parse_priority(const char *val, enum hs_priority *out, int lineno)
{
    if (!strcasecmp(val, "low"))    { *out = HS_PRIO_LOW;    return 0; }
    if (!strcasecmp(val, "normal")) { *out = HS_PRIO_NORMAL; return 0; }
    if (!strcasecmp(val, "high"))   { *out = HS_PRIO_HIGH;   return 0; }
    log_error("line %d: priority must be low|normal|high, got '%s'",
              lineno, val);
    return -1;
}

static int replace_string(char **slot, const char *val)
{
    char *copy = xstrdup(val);
    if (copy == NULL) {
        return -1;
    }
    free(*slot);
    *slot = copy;
    return 0;
}

/* ------------------------------------------------------------------ */
/* line tokenizer                                                     */
/*                                                                    */
/* Splits `line` in place into argv-style tokens. Recognises:         */
/*   - bare words separated by whitespace                             */
/*   - "double-quoted strings" (backslash escapes the closing quote   */
/*     and the backslash itself; other backslashes pass through)      */
/*   - '#' starts a comment that ends the line                        */
/* Returns the number of tokens, or -1 on a syntax error (unclosed    */
/* quote etc.). Modifies `line` by inserting NULs.                    */
/* ------------------------------------------------------------------ */

static int tokenize(char *line, char *tokens[], int max, int lineno)
{
    int n = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') break;

        if (n >= max) {
            log_error("line %d: too many tokens (limit %d)", lineno, max);
            return -1;
        }

        char *tok_start;
        if (*p == '"') {
            /* quoted: collapse \\ -> \ and \" -> " in place */
            p++;
            tok_start = p;
            char *write = p;
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                    *write++ = p[1];
                    p += 2;
                } else {
                    *write++ = *p++;
                }
            }
            if (*p != '"') {
                log_error("line %d: unterminated quoted string", lineno);
                return -1;
            }
            *write = '\0';
            p++;     /* past closing quote */
            tokens[n++] = tok_start;
        } else {
            tok_start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '#') p++;
            if (*p) *p++ = '\0';
            tokens[n++] = tok_start;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* watch options                                                      */
/* ------------------------------------------------------------------ */

static int append_watch(hs_config_t *cfg, hs_watch_t *w)
{
    size_t newn = cfg->n_watches + 1;
    hs_watch_t *ext = realloc(cfg->watches, newn * sizeof(*ext));
    if (ext == NULL) {
        log_error("out of memory growing watch list");
        return -1;
    }
    cfg->watches = ext;
    cfg->watches[cfg->n_watches] = *w;
    cfg->n_watches = newn;
    return 0;
}

/* Parse a single key=value option for a watch directive. Returns 0
 * on success, -1 on failure (already logged). */
static int parse_watch_option(hs_watch_t *w, const hs_config_t *cfg,
                              char *opt, int lineno)
{
    char *eq = strchr(opt, '=');
    if (eq == NULL) {
        log_error("line %d: watch option '%s' is not key=value", lineno, opt);
        return -1;
    }
    *eq = '\0';
    char *key = opt;
    char *val = eq + 1;

    /* Strip a pair of surrounding double quotes from the value, if
     * present. The tokenizer treats `key="value"` as one bareword
     * token (a `"` only triggers quote-mode when it starts a token);
     * the user-visible quoting in the example config is decorative
     * for the eye, and the literal `"` chars would otherwise leak
     * into the regex or string value. Values may not contain
     * whitespace — use whitespace-less regex constructs. */
    size_t vlen = strlen(val);
    if (vlen >= 2 && val[0] == '"' && val[vlen - 1] == '"') {
        val[vlen - 1] = '\0';
        val++;
    }

    if (!strcasecmp(key, "exclude")) {
        if (w->has_exclude) {
            log_error("line %d: duplicate exclude= on watch", lineno);
            return -1;
        }
        int rc = regcomp(&w->exclude, val, REG_EXTENDED | REG_NOSUB);
        if (rc != 0) {
            char errbuf[256];
            regerror(rc, &w->exclude, errbuf, sizeof(errbuf));
            log_error("line %d: exclude regex compile failed: %s",
                      lineno, errbuf);
            return -1;
        }
        w->has_exclude = true;
        return 0;
    }
    if (!strcasecmp(key, "recursive")) {
        return parse_bool(val, &w->recursive, "recursive=", lineno);
    }
    if (!strcasecmp(key, "retention")) {
        if (parse_duration_ns(val, &w->retention_ns) < 0) {
            log_error("line %d: retention= takes a duration like 30d or "
                      "'infinite', got '%s'", lineno, val);
            return -1;
        }
        w->set_retention = true;
        (void)cfg;
        return 0;
    }
    if (!strcasecmp(key, "priority")) {
        return parse_priority(val, &w->priority, lineno);
    }
    if (!strcasecmp(key, "compress")) {
        if (parse_bool(val, &w->compress, "compress=", lineno) < 0) return -1;
        w->set_compress = true;
        return 0;
    }
    log_error("line %d: unknown watch option '%s'", lineno, key);
    return -1;
}

static int handle_watch(hs_config_t *cfg, char *tokens[], int ntok, int lineno)
{
    if (ntok < 2) {
        log_error("line %d: watch directive needs a path", lineno);
        return -1;
    }

    /* Tentative values for retention_ns and compress are filled in
     * below from the current cfg state; if the watch does NOT set
     * them explicitly, config_load's post-pass overrides them with
     * the final retention-default / compress-default so directive
     * order in the config file does not matter. */
    hs_watch_t w = {
        .path           = NULL,
        .has_exclude    = false,
        .recursive      = true,
        .retention_ns   = cfg->retention_default_ns,
        .priority       = HS_PRIO_NORMAL,
        .compress       = cfg->compress_default,
        .set_retention  = false,
        .set_compress   = false,
    };

    w.path = xstrdup(tokens[1]);
    if (w.path == NULL) {
        return -1;
    }

    int rc = 0;
    for (int i = 2; i < ntok; i++) {
        if (parse_watch_option(&w, cfg, tokens[i], lineno) < 0) {
            rc = -1;
        }
    }

    if (rc < 0) {
        free(w.path);
        if (w.has_exclude) regfree(&w.exclude);
        return -1;
    }

    if (append_watch(cfg, &w) < 0) {
        free(w.path);
        if (w.has_exclude) regfree(&w.exclude);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* directive dispatch                                                 */
/* ------------------------------------------------------------------ */

static int dispatch(hs_config_t *cfg, char *tokens[], int ntok, int lineno)
{
    const char *dir = tokens[0];

    /* "single-value" string directives */
    if (!strcasecmp(dir, "storage")) {
        if (ntok != 2) goto need_one;
        return replace_string(&cfg->store_path, tokens[1]);
    }
    if (!strcasecmp(dir, "index")) {
        if (ntok != 2) goto need_one;
        return replace_string(&cfg->index_path, tokens[1]);
    }
    if (!strcasecmp(dir, "log")) {
        if (ntok != 2) goto need_one;
        return replace_string(&cfg->log_path, tokens[1]);
    }
    if (!strcasecmp(dir, "control-socket")) {
        if (ntok != 2) goto need_one;
        return replace_string(&cfg->control_socket, tokens[1]);
    }
    if (!strcasecmp(dir, "lock")) {
        if (ntok != 2) goto need_one;
        return replace_string(&cfg->lock_path, tokens[1]);
    }
    if (!strcasecmp(dir, "log-level")) {
        if (ntok != 2) goto need_one;
        return parse_log_level(tokens[1], &cfg->log_level, lineno);
    }
    if (!strcasecmp(dir, "fsync-mode")) {
        if (ntok != 2) goto need_one;
        int m = cfg->fsync_mode;
        if (parse_fsync_mode(tokens[1], &m, lineno) < 0) return -1;
        cfg->fsync_mode = m;
        return 0;
    }
    if (!strcasecmp(dir, "inotify-max-watches-warn")) {
        if (ntok != 2) goto need_one;
        return parse_uint(tokens[1], &cfg->inotify_max_watches_warn,
                          dir, lineno);
    }
    if (!strcasecmp(dir, "debounce-ms")) {
        if (ntok != 2) goto need_one;
        return parse_uint(tokens[1], &cfg->debounce_ms, dir, lineno);
    }
    if (!strcasecmp(dir, "queue-poll-ms")) {
        if (ntok != 2) goto need_one;
        return parse_uint(tokens[1], &cfg->queue_poll_ms, dir, lineno);
    }
    if (!strcasecmp(dir, "compress-default")) {
        if (ntok != 2) goto need_one;
        return parse_bool(tokens[1], &cfg->compress_default, dir, lineno);
    }
    if (!strcasecmp(dir, "compress-min-size")) {
        if (ntok != 2) goto need_one;
        return parse_size(tokens[1], &cfg->compress_min_size, dir, lineno);
    }
    if (!strcasecmp(dir, "store-fmode")) {
        if (ntok != 2) goto need_one;
        return parse_octal_mode(tokens[1], &cfg->store_fmode, dir, lineno);
    }
    if (!strcasecmp(dir, "retention-default")) {
        if (ntok != 2) goto need_one;
        if (parse_duration_ns(tokens[1], &cfg->retention_default_ns) < 0) {
            log_error("line %d: retention-default takes a duration "
                      "(e.g. 90d) or 'infinite', got '%s'",
                      lineno, tokens[1]);
            return -1;
        }
        return 0;
    }
    if (!strcasecmp(dir, "gc-after-prune")) {
        if (ntok != 2) goto need_one;
        return parse_bool(tokens[1], &cfg->gc_after_prune, dir, lineno);
    }

    /* multi-arg directive */
    if (!strcasecmp(dir, "watch")) {
        return handle_watch(cfg, tokens, ntok, lineno);
    }

    /* Tolerate unknown directives with a warning rather than failing
     * the whole load — the example config in etc/ already references
     * directives like auto-prune-schedule that we have not wired up. */
    log_warn("line %d: unknown directive '%s' (ignored)", lineno, dir);
    return 0;

need_one:
    log_error("line %d: %s expects exactly one argument", lineno, dir);
    return -1;
}

/* ------------------------------------------------------------------ */
/* file iteration with backslash-continuation                         */
/* ------------------------------------------------------------------ */

/* Read one logical line from `fp` into a heap buffer (caller frees).
 * Joins continuation lines (a backslash immediately before the
 * newline, optionally with trailing whitespace before the backslash).
 * Returns the number of source lines consumed (>=1) or 0 at EOF.
 * Stores the joined text (without the trailing newline) in *out. */
static int read_logical_line(FILE *fp, char **out, int *lineno_advance)
{
    char   *buf   = NULL;
    size_t  cap   = 0;
    size_t  len   = 0;
    int     lines = 0;

    for (;;) {
        char   *chunk = NULL;
        size_t  ccap  = 0;
        ssize_t got   = getline(&chunk, &ccap, fp);
        if (got < 0) {
            free(chunk);
            if (len == 0) {
                free(buf);
                *out = NULL;
                *lineno_advance = lines;
                return 0;
            }
            break;
        }
        lines++;

        /* drop a trailing \n if present */
        if (got > 0 && chunk[got - 1] == '\n') chunk[--got] = '\0';

        /* detect '\' line continuation: trailing backslash after
         * optional whitespace */
        bool cont = false;
        ssize_t i = got;
        while (i > 0 && (chunk[i - 1] == ' ' || chunk[i - 1] == '\t')) i--;
        if (i > 0 && chunk[i - 1] == '\\') {
            cont = true;
            chunk[--i] = '\0';
            got = i;
        }

        if (len + (size_t)got + 2 > cap) {
            size_t newcap = cap ? cap * 2 : 256;
            while (newcap < len + (size_t)got + 2) newcap *= 2;
            char *nb = realloc(buf, newcap);
            if (nb == NULL) {
                free(chunk);
                free(buf);
                log_error("out of memory reading config line");
                return -1;
            }
            buf = nb;
            cap = newcap;
        }
        if (len > 0) {
            buf[len++] = ' ';
        }
        memcpy(buf + len, chunk, (size_t)got);
        len += (size_t)got;
        buf[len] = '\0';
        free(chunk);

        if (!cont) break;
    }

    *out = buf;
    *lineno_advance = lines;
    return 1;
}

/* ------------------------------------------------------------------ */
/* public entry points                                                */
/* ------------------------------------------------------------------ */

hs_config_t *config_load(const char *path)
{
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        log_error("config: cannot open %s: %s", path, strerror(errno));
        return NULL;
    }

    hs_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg == NULL) {
        log_error("out of memory");
        fclose(fp);
        return NULL;
    }
    if (apply_defaults(cfg) < 0) {
        config_free(cfg);
        fclose(fp);
        return NULL;
    }

    int errors = 0;
    int lineno = 0;

    for (;;) {
        char *line = NULL;
        int   advance = 0;
        int   rc = read_logical_line(fp, &line, &advance);
        if (rc < 0) {
            errors++;
            break;
        }
        if (rc == 0) break;     /* EOF */
        int line_starts_at = lineno + 1;
        lineno += advance;

        char *tokens[MAX_TOKENS];
        int ntok = tokenize(line, tokens, MAX_TOKENS, line_starts_at);
        if (ntok < 0) {
            errors++;
            free(line);
            continue;
        }
        if (ntok == 0) {
            free(line);
            continue;
        }
        if (dispatch(cfg, tokens, ntok, line_starts_at) < 0) {
            errors++;
        }
        free(line);
    }

    fclose(fp);

    if (errors > 0) {
        log_error("config: %s had %d error%s, refusing to load",
                  path, errors, errors == 1 ? "" : "s");
        config_free(cfg);
        return NULL;
    }

    /* Apply final retention-default / compress-default to watches
     * that did not set those fields explicitly. Doing this after the
     * whole file is parsed means directive order in the config does
     * not matter. */
    for (size_t i = 0; i < cfg->n_watches; i++) {
        if (!cfg->watches[i].set_retention) {
            cfg->watches[i].retention_ns = cfg->retention_default_ns;
        }
        if (!cfg->watches[i].set_compress) {
            cfg->watches[i].compress = cfg->compress_default;
        }
    }

    return cfg;
}

void config_free(hs_config_t *cfg)
{
    if (cfg == NULL) return;

    free(cfg->store_path);
    free(cfg->index_path);
    free(cfg->log_path);
    free(cfg->control_socket);
    free(cfg->lock_path);

    for (size_t i = 0; i < cfg->n_watches; i++) {
        free(cfg->watches[i].path);
        if (cfg->watches[i].has_exclude) {
            regfree(&cfg->watches[i].exclude);
        }
    }
    free(cfg->watches);
    free(cfg);
}

/* ------------------------------------------------------------------ */
/* validation                                                         */
/* ------------------------------------------------------------------ */

static int check_dir_writable(const char *path, const char *what)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        log_error("config: %s path %s does not exist (%s)",
                  what, path, strerror(errno));
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        log_error("config: %s path %s is not a directory", what, path);
        return -1;
    }
    if (access(path, W_OK | X_OK) < 0) {
        log_error("config: %s path %s is not writable: %s",
                  what, path, strerror(errno));
        return -1;
    }
    return 0;
}

static int check_path_exists(const char *path, const char *what)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        log_error("config: %s path %s does not exist (%s)",
                  what, path, strerror(errno));
        return -1;
    }
    return 0;
}

int config_validate(const hs_config_t *cfg)
{
    if (cfg == NULL) {
        errno = EINVAL;
        return -1;
    }
    int errors = 0;
    if (check_dir_writable(cfg->store_path, "storage") < 0) errors++;
    if (check_dir_writable(cfg->index_path, "index") < 0) errors++;
    for (size_t i = 0; i < cfg->n_watches; i++) {
        if (check_path_exists(cfg->watches[i].path, "watch") < 0) errors++;
    }
    if (cfg->n_watches == 0) {
        log_warn("config: no watch directives — daemon will idle forever");
    }
    return errors == 0 ? 0 : -1;
}
