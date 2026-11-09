/* test_log.c — each promise and each error of log.c.
 *
 * The handle is opaque, so the test finds the descriptor of an open log in
 * /proc/self/fd, by the path that it points to. It can then check the flags
 * of the descriptor, and close it behind the back of the library.
 */

#undef NDEBUG               /* the asserts are the test */

#include "sentinel/syscall.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

#define LINE_MAX_BYTES 512

static char g_dir[PATH_MAX];

static void path_in(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "%s/%s", g_dir, name);
}

static int mode_of(const char *path)
{
    struct stat st;
    assert(stat(path, &st) == 0);
    return (int)(st.st_mode & 07777);
}

static long size_of(const char *path)
{
    struct stat st;
    assert(stat(path, &st) == 0);
    return (long)st.st_size;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f);
    assert(n >= 0);
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    assert(buf != NULL);
    assert(fread(buf, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    buf[n] = '\0';
    *len   = (size_t)n;
    return buf;
}

/* The lines of text, split in place. Each line must end with '\n'. */
static size_t split(char *text, size_t len, char **lines, size_t max)
{
    size_t n = 0;
    size_t start = 0;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') {
            assert(n < max);
            text[i]    = '\0';
            lines[n++] = text + start;
            start      = i + 1;
        }
    }
    assert(start == len);       /* no part of a line after the last '\n' */
    return n;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

/* The open descriptor that points to path, or -1. */
static int fd_of(const char *path)
{
    DIR *d = opendir("/proc/self/fd");
    assert(d != NULL);
    int            found = -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char link[300], target[PATH_MAX];
        if (e->d_name[0] == '.') {
            continue;
        }
        snprintf(link, sizeof link, "/proc/self/fd/%s", e->d_name);
        ssize_t n = readlink(link, target, sizeof target - 1);
        if (n < 0) {
            continue;
        }
        target[n] = '\0';
        if (strcmp(target, path) == 0) {
            found = atoi(e->d_name);
        }
    }
    closedir(d);
    return found;
}

static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    assert(d != NULL);
    int            n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.') {
            n++;
        }
    }
    closedir(d);
    return n - 1;               /* the descriptor of opendir */
}

/* Check the form "<seconds>.<6 digits> <LEVEL in 8 columns> <message>", and
   that the date is within 2 s of now. Returns the message. */
static const char *check_line(const char *line, const char *level)
{
    const char *p = line;
    assert(*p >= '0' && *p <= '9');
    char     *end = NULL;
    long long sec = strtoll(p, &end, 10);
    assert(*end == '.');
    p = end + 1;
    for (int i = 0; i < 6; i++) {
        assert(p[i] >= '0' && p[i] <= '9');
    }
    p += 6;
    assert(*p == ' ');
    p++;

    /* CLOCK_REALTIME, the clock of sc_stamp. time() can read a coarse clock
       that is still in the last second for a few milliseconds, and a stamp
       then looks as if it came from the future. */
    struct timespec ts;
    assert(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    long long now = (long long)ts.tv_sec;
    assert(sec >= now - 2 && sec <= now);

    char field[16];
    snprintf(field, sizeof field, "%-8s ", level);
    assert(strncmp(p, field, 9) == 0);
    return p + 9;
}

/* ---- log_level_name ------------------------------------------------------ */

static void test_level_names(void)
{
    static const char *const names[] = { "SYSTEM", "INFO", "PENDING", "PRIORITY", "ALERT", "TRACK" };
    assert(strcmp(log_level_name(LOG_SYSTEM), "SYSTEM") == 0);
    assert(strcmp(log_level_name(LOG_TRACK), "TRACK") == 0);
    for (int i = 0; i < 6; i++) {
        const char *got = log_level_name((log_level_t)i);
        assert(got != NULL && strcmp(got, names[i]) == 0);
    }

    /* Not a log_level_t: NULL, and errno does not change. */
    errno = EDOM;
    assert(log_level_name((log_level_t)6) == NULL);
    assert(log_level_name((log_level_t)99) == NULL);
    assert(log_level_name((log_level_t)-1) == NULL);
    assert(errno == EDOM);

    assert(strcmp(log_level_name(LOG_INFO), "INFO") == 0);
}

/* ---- log_open ------------------------------------------------------------ */

static void test_open_mode(void)
{
    char p[PATH_MAX + 16];
    path_in(p, sizeof p, "mode.log");
    mode_t old = umask(0);

    /* A new log is 0600, also when the umask allows more. */
    unlink(p);
    log_t *lg = log_open(p);
    assert(lg != NULL);
    assert(log_close(lg) == 0);
    assert(mode_of(p) == 0600);

    /* An old log with other permissions becomes 0600. */
    static const int modes[] = { 0644, 0666, 0400, 0700 };
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        assert(chmod(p, (mode_t)modes[i]) == 0);
        lg = log_open(p);
        if (modes[i] == 0400 && geteuid() != 0) {
            /* The owner cannot write a file of mode 0400, so open() fails
               before fchmod() runs. */
            assert(lg == NULL && errno == EACCES);
            continue;
        }
        assert(lg != NULL);
        assert(mode_of(p) == 0600);
        assert(log_close(lg) == 0);
    }

    /* A umask that removes the owner's bits: fchmod() still sets 0600. */
    umask(0777);
    unlink(p);
    lg = log_open(p);
    assert(lg != NULL);
    assert(mode_of(p) == 0600);
    assert(log_close(lg) == 0);

    umask(old);
}

static void test_open_appends(void)
{
    char p[PATH_MAX + 16];
    path_in(p, sizeof p, "append.log");
    put(p, "an old line\n");

    int    fds = count_fds();
    log_t *lg  = log_open(p);
    assert(lg != NULL);
    assert(count_fds() == fds + 1);

    /* Open for writing, with O_APPEND. */
    int fd = fd_of(p);
    assert(fd >= 0);
    int flags = fcntl(fd, F_GETFL);
    assert((flags & O_ACCMODE) == O_WRONLY);
    assert((flags & O_APPEND) != 0);

    assert(log_writef(lg, LOG_INFO, "first") == 0);

    /* A different writer adds a line. The next line of the log goes after
       it, not over it. */
    FILE *other = fopen(p, "a");
    assert(other != NULL);
    assert(fputs("another writer\n", other) >= 0);
    assert(fclose(other) == 0);

    assert(log_writef(lg, LOG_INFO, "second") == 0);
    assert(log_close(lg) == 0);
    assert(count_fds() == fds);

    size_t len;
    char  *text = slurp(p, &len);
    char  *lines[8];
    size_t n = split(text, len, lines, 8);
    assert(n == 4);
    assert(strcmp(lines[0], "an old line") == 0);
    assert(strcmp(check_line(lines[1], "INFO"), "first") == 0);
    assert(strcmp(lines[2], "another writer") == 0);
    assert(strcmp(check_line(lines[3], "INFO"), "second") == 0);
    free(text);

    /* A second log_open keeps each line. */
    lg = log_open(p);
    assert(lg != NULL);
    assert(log_writef(lg, LOG_SYSTEM, "third") == 0);
    assert(log_close(lg) == 0);
    text = slurp(p, &len);
    n    = split(text, len, lines, 8);
    assert(n == 5);
    assert(strcmp(check_line(lines[4], "SYSTEM"), "third") == 0);
    free(text);
}

/* ---- log_writef ---------------------------------------------------------- */

static void test_format(void)
{
    char p[PATH_MAX + 16];
    path_in(p, sizeof p, "format.log");
    unlink(p);

    log_t *lg = log_open(p);
    assert(lg != NULL);
    assert(log_writef(lg, LOG_SYSTEM, "SENTINEL starting") == 0);
    assert(log_writef(lg, LOG_INFO, "contact %s from %s", "ACCF-4587", "NWS") == 0);
    assert(log_writef(lg, LOG_PENDING, "queued %d", 42) == 0);
    assert(log_writef(lg, LOG_PRIORITY, "at %.4f,%.4f speed %.1f", 68.71, -105.7, 420.0) == 0);
    assert(log_writef(lg, LOG_ALERT, "%s", "") == 0);
    assert(log_writef(lg, LOG_TRACK, "100%% %c", 'x') == 0);
    assert(log_close(lg) == 0);

    static const char *const level[] = { "SYSTEM", "INFO", "PENDING", "PRIORITY", "ALERT", "TRACK" };
    static const char *const msg[]   = {
        "SENTINEL starting", "contact ACCF-4587 from NWS", "queued 42",
        "at 68.7100,-105.7000 speed 420.0", "", "100% x",
    };
    size_t len;
    char  *text = slurp(p, &len);
    char  *lines[8];
    assert(split(text, len, lines, 8) == 6);
    for (int i = 0; i < 6; i++) {
        assert(strcmp(check_line(lines[i], level[i]), msg[i]) == 0);
    }
    free(text);
}

static void test_long_and_newline(void)
{
    char p[PATH_MAX + 16];
    path_in(p, sizeof p, "long.log");
    unlink(p);

    char huge[4096];
    memset(huge, 'Z', sizeof huge - 1);
    huge[sizeof huge - 1] = '\0';

    log_t *lg = log_open(p);
    assert(lg != NULL);
    assert(log_writef(lg, LOG_INFO, "%s", huge) == 0);
    assert(log_writef(lg, LOG_INFO, "after a long line") == 0);
    assert(log_writef(lg, LOG_INFO, "a\nb\n") == 0);
    assert(log_close(lg) == 0);

    size_t len;
    char  *text = slurp(p, &len);
    char  *nl   = memchr(text, '\n', len);
    assert(nl != NULL);
    /* The long message is cut at its end, and its line is 512 bytes, with
       its '\n'. */
    assert((size_t)(nl - text) + 1 == LINE_MAX_BYTES);
    char  *lines[8];
    assert(split(text, len, lines, 8) == 3);
    const char *m = check_line(lines[0], "INFO");
    for (const char *q = m; *q != '\0'; q++) {
        assert(*q == 'Z');
    }
    assert(strcmp(check_line(lines[1], "INFO"), "after a long line") == 0);
    /* Each '\n' becomes a space, also the last one: one call, one line. */
    assert(strcmp(check_line(lines[2], "INFO"), "a b ") == 0);
    size_t head = (size_t)(m - lines[0]);
    free(text);

    /* A message that just fits is not cut. One more byte is cut. */
    size_t fit = LINE_MAX_BYTES - 1 - head;
    char   msg[LINE_MAX_BYTES + 2];
    memset(msg, 'a', fit);
    msg[fit]     = 'b';
    msg[fit + 1] = '\0';
    unlink(p);
    lg = log_open(p);
    assert(lg != NULL);
    assert(log_writef(lg, LOG_INFO, "%.*s", (int)fit, msg) == 0);
    assert(log_writef(lg, LOG_INFO, "%s", msg) == 0);
    assert(log_close(lg) == 0);
    text = slurp(p, &len);
    assert(len == 2 * LINE_MAX_BYTES);
    assert(split(text, len, lines, 8) == 2);
    m = check_line(lines[0], "INFO");
    assert(strlen(m) == fit && strspn(m, "a") == fit);
    m = check_line(lines[1], "INFO");
    assert(strlen(m) == fit && strspn(m, "a") == fit);
    free(text);
}

static void test_writef_errors(void)
{
    char p[PATH_MAX + 16];
    path_in(p, sizeof p, "errors.log");
    unlink(p);
    log_t *lg = log_open(p);
    assert(lg != NULL);
    assert(log_writef(lg, LOG_INFO, "start") == 0);
    long size = size_of(p);

    errno = 0;
    assert(log_writef(NULL, LOG_INFO, "x") == -1 && errno == EINVAL);

    /* A pointer that the compiler cannot see is NULL, and one argument after
       it, so that the format checks of -Wformat do not stop the build. */
    const char *volatile nofmt = NULL;
    errno = 0;
    assert(log_writef(lg, LOG_INFO, nofmt, 0) == -1 && errno == EINVAL);

    errno = 0;
    assert(log_writef(lg, (log_level_t)6, "bad level") == -1 && errno == EINVAL);
    errno = 0;
    assert(log_writef(lg, (log_level_t)99, "bad level") == -1 && errno == EINVAL);
    errno = 0;
    assert(log_writef(lg, (log_level_t)-1, "bad level") == -1 && errno == EINVAL);

    /* A conversion that vsnprintf() cannot do: in the C locale, a wide
       character above 0x7f has no multibyte form. */
    errno = 0;
    assert(log_writef(lg, LOG_INFO, "%lc", (wint_t)0x100) == -1 && errno == EOVERFLOW);

    assert(size_of(p) == size);          /* each error wrote nothing */
    assert(log_writef(lg, LOG_INFO, "still works") == 0);
    assert(size_of(p) > size);

    /* A write that fails: the file cannot grow past RLIMIT_FSIZE. With
       SIGXFSZ ignored, write() fails with EFBIG. */
    struct rlimit old;
    assert(getrlimit(RLIMIT_FSIZE, &old) == 0);
    void (*old_handler)(int) = signal(SIGXFSZ, SIG_IGN);
    struct rlimit small = old;
    small.rlim_cur = (rlim_t)size_of(p) + 40;
    assert(setrlimit(RLIMIT_FSIZE, &small) == 0);
    char pad[200];
    memset(pad, 'p', sizeof pad - 1);
    pad[sizeof pad - 1] = '\0';
    errno = 0;
    assert(log_writef(lg, LOG_INFO, "%s", pad) == -1 && errno == EFBIG);
    assert(setrlimit(RLIMIT_FSIZE, &old) == 0);
    signal(SIGXFSZ, old_handler);

    /* The next call still works. */
    assert(log_writef(lg, LOG_INFO, "after EFBIG") == 0);
    assert(log_close(lg) == 0);
}

/* ---- the errors of log_open and log_close -------------------------------- */

static void test_open_close_errors(void)
{
    char p[PATH_MAX + 32];
    int  fds = count_fds();

    errno = 0;
    assert(log_open(NULL) == NULL && errno == EINVAL);

    path_in(p, sizeof p, "no-such-dir/x.log");
    errno = 0;
    assert(log_open(p) == NULL && errno == ENOENT);

    errno = 0;
    assert(log_open(g_dir) == NULL && errno == EISDIR);

    /* fchmod() fails on a file of another user: the log is closed, and the
       errno is the errno of fchmod(). */
    struct stat st;
    if (geteuid() != 0 && stat("/dev/full", &st) == 0 && st.st_uid != geteuid()) {
        errno = 0;
        assert(log_open("/dev/full") == NULL && errno == EPERM);
    }
    assert(count_fds() == fds);          /* no descriptor stays open */

    assert(log_close(NULL) == 0);

    /* A close() that fails: -1 with its errno. The handle is still freed,
       which valgrind checks. */
    path_in(p, sizeof p, "close.log");
    log_t *lg = log_open(p);
    assert(lg != NULL);
    int fd = fd_of(p);
    assert(fd >= 0);
    assert(close(fd) == 0);
    errno = 0;
    assert(log_close(lg) == -1 && errno == EBADF);

    /* The next calls still work. */
    lg = log_open(p);
    assert(lg != NULL);
    assert(log_writef(lg, LOG_INFO, "still works") == 0);
    assert(log_close(lg) == 0);
    assert(count_fds() == fds);
}

/* ---- many writers -------------------------------------------------------- */

static void test_concurrent(void)
{
    enum { WRITERS = 4, LINES = 400 };
    char p[PATH_MAX + 16];
    path_in(p, sizeof p, "concurrent.log");
    unlink(p);

    fflush(NULL);
    for (int w = 0; w < WRITERS; w++) {
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            log_t *lg = log_open(p);
            if (lg == NULL) {
                _exit(2);
            }
            for (int i = 0; i < LINES; i++) {
                if (log_writef(lg, LOG_TRACK, "writer=%d seq=%04d %s", w, i,
                               "................................................") != 0) {
                    _exit(3);
                }
            }
            _exit(log_close(lg) == 0 ? 0 : 4);
        }
    }
    for (int w = 0; w < WRITERS; w++) {
        int status = 0;
        assert(wait(&status) > 0);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    size_t len;
    char  *text  = slurp(p, &len);
    char **lines = malloc(WRITERS * LINES * sizeof *lines);
    assert(lines != NULL);
    assert(split(text, len, lines, WRITERS * LINES) == WRITERS * LINES);

    /* Each line is whole, and each writer's lines are in its order. */
    int next[WRITERS] = { 0 };
    for (int i = 0; i < WRITERS * LINES; i++) {
        const char *m = check_line(lines[i], "TRACK");
        int         w, seq, used = 0;
        assert(sscanf(m, "writer=%d seq=%d %n", &w, &seq, &used) == 2);
        assert(w >= 0 && w < WRITERS);
        assert(seq == next[w]);
        assert(strcmp(m + used, "................................................") == 0);
        next[w]++;
    }
    free(lines);
    free(text);
}

static void remove_dir(void)
{
    static const char *const names[] = {
        "mode.log", "append.log", "format.log", "long.log", "errors.log",
        "close.log", "concurrent.log",
    };
    char p[PATH_MAX + 16];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        path_in(p, sizeof p, names[i]);
        unlink(p);
    }
    assert(rmdir(g_dir) == 0);
}

int main(void)
{
    char tmpl[] = "/tmp/sentinel_test_log_XXXXXX";
    assert(mkdtemp(tmpl) != NULL);
    /* /proc/self/fd gives the real path, so the test uses it too. */
    assert(realpath(tmpl, g_dir) != NULL);

    test_level_names();
    test_open_mode();
    test_open_appends();
    test_format();
    test_long_and_newline();
    test_writef_errors();
    test_open_close_errors();
    test_concurrent();

    remove_dir();
    printf("test_log: each assertion passes\n");
    return 0;
}
