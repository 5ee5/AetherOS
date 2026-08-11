#include "pwdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Whole-file working buffers. Both files are line-oriented and tiny, so they are
   loaded, edited and written back in one shot. */
#define DB_MAX 8192

static char s_load[DB_MAX];
static char s_out[DB_MAX];

/* Read `path` entirely into s_load.  Returns the byte count, or -1 on error.
 *
 * A file that fills the buffer is an error rather than a silent truncation: the
 * rewrite paths below write back what they read, so truncating here would delete
 * every account past the buffer — which is what the old per-tool 2 KiB parsers
 * did. */
static long db_load(const char *path)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    long n = read(fd, s_load, DB_MAX - 1);
    close(fd);
    if (n < 0) return -1;
    if (n >= DB_MAX - 1) return -1;     /* too large to edit safely */
    s_load[n] = '\0';
    return n;
}

static int db_store(const char *path, const char *data, long len)
{
    int fd = open(path, O_WRONLY | O_TRUNC | O_CREAT, 0);
    if (fd < 0) return -1;
    long w = write(fd, data, (size_t)len);
    close(fd);
    return (w == len) ? 0 : -1;
}

/* Consume one line from *cursor, NUL-terminating it in place.  Returns NULL at
   end of buffer. */
static char *next_line(char **cursor)
{
    char *p = *cursor;
    if (p == NULL || *p == '\0') return NULL;

    char *nl = strchr(p, '\n');
    if (nl) {
        *nl = '\0';
        *cursor = nl + 1;
    } else {
        *cursor = p + strlen(p);
    }
    return p;
}

/* Split `line` in place into at most `maxf` colon-separated fields.
   Returns the number of fields found. */
static int split(char *line, char **f, int maxf)
{
    int n = 0;
    char *tok = line;
    while (n < maxf) {
        f[n++] = tok;
        char *sep = strchr(tok, ':');
        if (!sep) break;
        *sep = '\0';
        tok = sep + 1;
    }
    return n;
}

/* Copy the first colon-separated field of `line` into out[outsz]. */
static void first_field(const char *line, char *out, int outsz)
{
    int i = 0;
    while (line[i] && line[i] != ':' && i < outsz - 1) {
        out[i] = line[i];
        i++;
    }
    out[i] = '\0';
}

static void fill_ent(pwent_t *out, char **f)
{
    strncpy(out->name, f[0], PW_NAME_MAX - 1);
    out->name[PW_NAME_MAX - 1] = '\0';
    out->uid = (uid_t)atoi(f[2]);
    out->gid = (gid_t)atoi(f[3]);
    strncpy(out->home, f[4], PW_HOME_MAX - 1);
    out->home[PW_HOME_MAX - 1] = '\0';
}

int pw_by_name(const char *name, pwent_t *out)
{
    if (db_load(PASSWD_PATH) < 0) return -1;

    char *cur = s_load, *line;
    while ((line = next_line(&cur)) != NULL) {
        char *f[5];
        if (split(line, f, 5) < 5) continue;
        if (strcmp(f[0], name) == 0) { fill_ent(out, f); return 0; }
    }
    return -1;
}

int pw_by_uid(uid_t uid, pwent_t *out)
{
    if (db_load(PASSWD_PATH) < 0) return -1;

    char *cur = s_load, *line;
    while ((line = next_line(&cur)) != NULL) {
        char *f[5];
        if (split(line, f, 5) < 5) continue;
        if ((uid_t)atoi(f[2]) == uid) { fill_ent(out, f); return 0; }
    }
    return -1;
}

uid_t pw_max_uid(void)
{
    if (db_load(PASSWD_PATH) < 0) return 0;

    uid_t max = 0;
    char *cur = s_load, *line;
    while ((line = next_line(&cur)) != NULL) {
        char *f[5];
        if (split(line, f, 5) < 5) continue;
        uid_t u = (uid_t)atoi(f[2]);
        if (u > max && u < 60000U) max = u;
    }
    return max;
}

int pw_add(const pwent_t *e)
{
    pwent_t existing;
    if (pw_by_name(e->name, &existing) == 0) return -1;

    long n = db_load(PASSWD_PATH);
    if (n < 0) return -1;

    if (n > 0 && s_load[n - 1] != '\n') s_load[n++] = '\n';
    if (n > 0) memcpy(s_out, s_load, (size_t)n);

    int len = snprintf(s_out + n, (size_t)(DB_MAX - n), "%s:x:%u:%u:%s\n",
                       e->name, e->uid, e->gid, e->home);
    if (len <= 0 || n + len >= DB_MAX) return -1;
    return db_store(PASSWD_PATH, s_out, n + len);
}

/* Rewrite `path` without the line whose first field is `name`.
   Returns 0 if a line was removed, -1 otherwise. */
static int db_drop_line(const char *path, const char *name)
{
    if (db_load(path) < 0) return -1;

    long out = 0;
    int found = 0;
    char *cur = s_load, *line;
    while ((line = next_line(&cur)) != NULL) {
        char first[PW_NAME_MAX];
        first_field(line, first, sizeof(first));
        if (strcmp(first, name) == 0) { found = 1; continue; }

        int len = (int)strlen(line);
        if (out + len + 1 >= DB_MAX) return -1;
        memcpy(s_out + out, line, (size_t)len);
        out += len;
        s_out[out++] = '\n';
    }
    if (!found) return -1;
    return db_store(path, s_out, out);
}

int pw_del(const char *name)
{
    if (db_drop_line(PASSWD_PATH, name) != 0) return -1;
    /* A missing shadow entry is not an error: the account is gone either way. */
    (void)db_drop_line(SHADOW_PATH, name);
    return 0;
}

int sh_get(const char *name, char *hash, int hashsz)
{
    if (hashsz <= 0) return -1;
    hash[0] = '\0';
    if (db_load(SHADOW_PATH) < 0) return -1;

    char *cur = s_load, *line;
    while ((line = next_line(&cur)) != NULL) {
        char *f[2];
        if (split(line, f, 2) < 2) continue;
        if (strcmp(f[0], name) != 0) continue;
        strncpy(hash, f[1], (size_t)hashsz - 1);
        hash[hashsz - 1] = '\0';
        return 0;
    }
    return -1;
}

int sh_set(const char *name, const char *hash)
{
    if (db_load(SHADOW_PATH) < 0) s_load[0] = '\0';   /* absent: create it */

    long out = 0;
    int found = 0;
    char *cur = s_load, *line;

    while ((line = next_line(&cur)) != NULL) {
        char first[PW_NAME_MAX];
        first_field(line, first, sizeof(first));

        int len;
        if (strcmp(first, name) == 0) {
            found = 1;
            len = snprintf(s_out + out, (size_t)(DB_MAX - out), "%s:%s\n",
                           name, hash);
        } else {
            len = snprintf(s_out + out, (size_t)(DB_MAX - out), "%s\n", line);
        }
        if (len <= 0 || out + len >= DB_MAX) return -1;
        out += len;
    }

    if (!found) {
        int len = snprintf(s_out + out, (size_t)(DB_MAX - out), "%s:%s\n",
                           name, hash);
        if (len <= 0 || out + len >= DB_MAX) return -1;
        out += len;
    }

    return db_store(SHADOW_PATH, s_out, out);
}
