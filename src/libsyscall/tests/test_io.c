/* test_io.c — each promise and each error of io.c.
 *
 * On a regular file the kernel almost always moves all the bytes in one call,
 * and a signal almost never arrives in the middle of one. So a wrapper with
 * no retry loop passes a test that only copies a file. This program defines
 * read(), write() and close(), and the library is linked to these
 * definitions. A test can then make a call move part of the bytes, fail with
 * EINTR, return 0, or fail after some bytes. The system call itself goes
 * through syscall(), which does not come back here.
 *
 * Two tests use real signals as well: a SIGALRM with no SA_RESTART stops an
 * open() of a FIFO, and the read() and write() of a pipe.
 */

#undef _FORTIFY_SOURCE      /* its inline read() would clash with the one below */
#undef NDEBUG               /* the asserts are the test */

#include "sentinel/syscall.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BIG (1024 * 1024)

static char g_dir[64];

/* ---- fault injection --------------------------------------------------- */

enum fault {
    F_NONE,
    F_SHORT_EINTR,      /* at most 777 bytes, and each third call EINTR     */
    F_ZERO,             /* return 0 and move nothing                        */
    F_FAIL_AFTER,       /* move g_limit bytes, then fail with g_fail_errno  */
    F_CLOSE_EINTR       /* close() closes, then reports EINTR               */
};

static enum fault g_fault;
static int        g_calls;      /* calls that saw the fault */
static size_t     g_moved;
static size_t     g_limit;
static int        g_fail_errno;

static void inject(enum fault f)
{
    g_fault = f;
    g_calls = 0;
    g_moved = 0;
}

static ssize_t injected(long number, int fd, void *buf, size_t n)
{
    if (n > 0 && g_fault != F_NONE && g_fault != F_CLOSE_EINTR) {
        g_calls++;
        if (g_fault == F_SHORT_EINTR) {
            if (g_calls % 3 == 0) {
                errno = EINTR;
                return -1;
            }
            if (n > 777) {
                n = 777;
            }
        } else if (g_fault == F_ZERO) {
            return 0;
        } else if (g_fault == F_FAIL_AFTER) {
            if (g_moved >= g_limit) {
                errno = g_fail_errno;
                return -1;
            }
            if (n > g_limit - g_moved) {
                n = g_limit - g_moved;
            }
            long r = syscall(number, fd, buf, n);
            if (r > 0) {
                g_moved += (size_t)r;
            }
            return r;
        }
    }
    return syscall(number, fd, buf, n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
    return injected(SYS_write, fd, (void *)buf, n);
}

ssize_t read(int fd, void *buf, size_t n)
{
    return injected(SYS_read, fd, buf, n);
}

int close(int fd)
{
    if (g_fault == F_CLOSE_EINTR) {
        g_calls++;
        if (syscall(SYS_close, fd) == 0) {
            errno = EINTR;
            return -1;
        }
        return -1;
    }
    return (int)syscall(SYS_close, fd);
}

/* ---- helpers, with the C library only ----------------------------------- */

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

static int fd_is_open(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

/* The whole file, and its length. stdio reads with the internal read() of
   the C library, which the injection does not reach. */
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    char  *buf = malloc(2 * BIG);
    assert(buf != NULL);
    size_t n = fread(buf, 1, 2 * BIG, f);
    assert(ferror(f) == 0);
    fclose(f);
    *len = n;
    return buf;
}

static void put(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(data, 1, len, f) == len);
    assert(fclose(f) == 0);
}

static char *pattern(int seed)
{
    char *buf = malloc(BIG);
    assert(buf != NULL);
    for (int i = 0; i < BIG; i++) {
        buf[i] = (char)(i * seed + 7);
    }
    return buf;
}

static void sleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

static volatile sig_atomic_t g_alarms;

static void on_alarm(int sig)
{
    (void)sig;
    g_alarms++;
}

/* SIGALRM with no SA_RESTART, so that a system call that waits fails with
   EINTR. With SA_RESTART, the kernel would restart it, and the retry of the
   library would not run. */
static void catch_alarm(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    assert(sigaction(SIGALRM, &sa, NULL) == 0);
}

static void alarm_every_us(long first, long every)
{
    struct itimerval it = { { 0, every }, { 0, first } };
    assert(setitimer(ITIMER_REAL, &it, NULL) == 0);
}

/* ---- sc_open_write and sc_open_write_mode -------------------------------- */

static void test_open_write_modes(void)
{
    char p[128];
    path_in(p, sizeof p, "mode.txt");
    mode_t old = umask(0);

    unlink(p);
    int fd = sc_open_write(p, 0);
    assert(fd >= 0);
    assert(mode_of(p) == 0644);
    int flags = fcntl(fd, F_GETFL);
    assert((flags & O_ACCMODE) == O_WRONLY);
    assert((flags & O_APPEND) == 0);
    assert(sc_close(fd) == 0);

    unlink(p);
    fd = sc_open_write(p, 1);
    assert(fd >= 0);
    flags = fcntl(fd, F_GETFL);
    assert((flags & O_ACCMODE) == O_WRONLY);
    assert((flags & O_APPEND) != 0);
    assert(sc_close(fd) == 0);

    unlink(p);
    fd = sc_open_write_mode(p, 0, 0600);
    assert(fd >= 0);
    assert(mode_of(p) == 0600);
    assert(sc_close(fd) == 0);

    unlink(p);
    fd = sc_open_write_mode(p, 1, 0640);
    assert(fd >= 0);
    assert(mode_of(p) == 0640);
    assert((fcntl(fd, F_GETFL) & O_APPEND) != 0);
    assert(sc_close(fd) == 0);

    /* The umask masks the mode, as it does for open(): the example of
       syscall.h. */
    umask(027);
    unlink(p);
    fd = sc_open_write(p, 0);
    assert(fd >= 0);
    assert(mode_of(p) == 0640);
    assert(sc_close(fd) == 0);
    unlink(p);
    fd = sc_open_write_mode(p, 0, 0666);
    assert(fd >= 0);
    assert(mode_of(p) == 0640);
    assert(sc_close(fd) == 0);

    /* The mode of a file that exists already does not change. */
    assert(chmod(p, 0600) == 0);
    fd = sc_open_write(p, 1);
    assert(fd >= 0);
    assert(sc_close(fd) == 0);
    assert(mode_of(p) == 0600);
    fd = sc_open_write_mode(p, 0, 0644);
    assert(fd >= 0);
    assert(sc_close(fd) == 0);
    assert(mode_of(p) == 0600);

    umask(old);
}

static void test_append_truncate(void)
{
    char p[128];
    path_in(p, sizeof p, "append.txt");
    unlink(p);

    int fd = sc_open_write(p, 1);
    assert(fd >= 0);
    assert(sc_write_all(fd, "first\n", 6) == 0);
    assert(sc_close(fd) == 0);

    fd = sc_open_write(p, 1);
    assert(fd >= 0);
    assert(sc_write_all(fd, "second\n", 7) == 0);
    assert(sc_close(fd) == 0);

    size_t len;
    char  *text = slurp(p, &len);
    assert(len == 13 && memcmp(text, "first\nsecond\n", 13) == 0);
    free(text);

    fd = sc_open_write(p, 0);
    assert(fd >= 0);
    assert(sc_close(fd) == 0);
    text = slurp(p, &len);
    assert(len == 0);
    free(text);

    /* append == 0 also removes the content of a file that sc_open_write_mode
       opens. */
    put(p, "old", 3);
    fd = sc_open_write_mode(p, 0, 0600);
    assert(fd >= 0);
    assert(sc_write_all(fd, "new", 3) == 0);
    assert(sc_close(fd) == 0);
    text = slurp(p, &len);
    assert(len == 3 && memcmp(text, "new", 3) == 0);
    free(text);
}

/* Two descriptors of the same file, from two opens, as two processes have.
   Each keeps its own offset. With O_APPEND, each write still goes to the end
   of the file, so no write goes over another. */
static void test_append_two_writers(void)
{
    char p[128];
    path_in(p, sizeof p, "two.txt");
    unlink(p);

    int a = sc_open_write(p, 1);
    int b = sc_open_write(p, 1);
    assert(a >= 0 && b >= 0);
    assert(sc_write_all(a, "aaaa\n", 5) == 0);
    assert(sc_write_all(b, "bbbb\n", 5) == 0);
    assert(sc_write_all(a, "cccc\n", 5) == 0);
    assert(sc_close(a) == 0);
    assert(sc_close(b) == 0);

    size_t len;
    char  *text = slurp(p, &len);
    assert(len == 15 && memcmp(text, "aaaa\nbbbb\ncccc\n", 15) == 0);
    free(text);
}

/* ---- sc_open_read, and the errors of each open --------------------------- */

static void test_open_read(void)
{
    char p[128], q[160];
    path_in(p, sizeof p, "read.txt");
    put(p, "hello\n", 6);

    int fd = sc_open_read(p);
    assert(fd >= 0);
    assert((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDONLY);
    char buf[16];
    assert(sc_read_full(fd, buf, sizeof buf) == 6 && memcmp(buf, "hello\n", 6) == 0);
    /* Open for reading only. */
    errno = 0;
    assert(sc_write_all(fd, "x", 1) == -1 && errno == EBADF);
    assert(sc_close(fd) == 0);

    errno = 0;
    path_in(q, sizeof q, "missing.txt");
    assert(sc_open_read(q) == -1 && errno == ENOENT);
    fd = sc_open_read(p);
    assert(fd >= 0);
    assert(sc_close(fd) == 0);

    errno = 0;
    snprintf(q, sizeof q, "%s/below", p);
    assert(sc_open_read(q) == -1 && errno == ENOTDIR);

    /* root reads each file, whatever its mode. */
    if (geteuid() != 0) {
        assert(chmod(p, 0) == 0);
        errno = 0;
        assert(sc_open_read(p) == -1 && errno == EACCES);
        assert(chmod(p, 0644) == 0);
        fd = sc_open_read(p);
        assert(fd >= 0);
        assert(sc_close(fd) == 0);
    }
}

static void test_open_write_errors(void)
{
    char p[160];
    path_in(p, sizeof p, "no-such-dir/x.txt");
    errno = 0;
    assert(sc_open_write(p, 0) == -1 && errno == ENOENT);
    errno = 0;
    assert(sc_open_write_mode(p, 1, 0600) == -1 && errno == ENOENT);

    errno = 0;
    assert(sc_open_write(g_dir, 1) == -1 && errno == EISDIR);
    errno = 0;
    assert(sc_open_write_mode(g_dir, 0, 0600) == -1 && errno == EISDIR);

    path_in(p, sizeof p, "after-error.txt");
    int fd = sc_open_write(p, 0);
    assert(fd >= 0);
    assert(sc_write_all(fd, "ok", 2) == 0);
    assert(sc_close(fd) == 0);
}

/* open() of a FIFO waits until the other end opens. A signal in that wait
   stops open() with EINTR, and the library must open again. */
static void test_open_eintr(void)
{
    char p[128];
    path_in(p, sizeof p, "fifo");
    unlink(p);
    assert(mkfifo(p, 0600) == 0);
    catch_alarm();

    /* sc_open_read waits for a writer. */
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        sleep_ms(300);
        int w = open(p, O_WRONLY);
        _exit(w >= 0 && write(w, "fifo\n", 5) == 5 ? 0 : 1);
    }
    g_alarms = 0;
    alarm_every_us(50000, 0);
    int fd = sc_open_read(p);
    assert(fd >= 0);
    assert(g_alarms >= 1);
    char buf[8];
    assert(sc_read_full(fd, buf, sizeof buf) == 5 && memcmp(buf, "fifo\n", 5) == 0);
    assert(sc_close(fd) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* sc_open_write waits for a reader. */
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        sleep_ms(300);
        int r = open(p, O_RDONLY);
        char got[8];
        _exit(r >= 0 && read(r, got, sizeof got) == 3 && memcmp(got, "go\n", 3) == 0 ? 0 : 1);
    }
    g_alarms = 0;
    alarm_every_us(50000, 0);
    fd = sc_open_write(p, 0);
    assert(fd >= 0);
    assert(g_alarms >= 1);
    assert(sc_write_all(fd, "go\n", 3) == 0);
    assert(sc_close(fd) == 0);
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* ---- sc_write_all -------------------------------------------------------- */

static void test_write_all(void)
{
    char p[128];
    path_in(p, sizeof p, "write.bin");
    char *out = pattern(31);

    /* Partial writes and EINTR: each byte arrives, in order. */
    int fd = sc_open_write(p, 0);
    assert(fd >= 0);
    inject(F_SHORT_EINTR);
    int rc = sc_write_all(fd, out, BIG);
    int calls = g_calls;
    inject(F_NONE);
    assert(rc == 0);
    assert(calls > BIG / 777);
    assert(sc_close(fd) == 0);
    size_t len;
    char  *disk = slurp(p, &len);
    assert(len == BIG && memcmp(disk, out, BIG) == 0);
    free(disk);

    /* n == 0 writes nothing, whatever fd is. */
    inject(F_SHORT_EINTR);
    assert(sc_write_all(-1, out, 0) == 0);
    assert(sc_write_all(-1, NULL, 0) == 0);
    assert(g_calls == 0);
    inject(F_NONE);

    errno = 0;
    assert(sc_write_all(-1, out, 4) == -1 && errno == EBADF);

    /* write() returns 0: EIO, and the loop ends. */
    fd = sc_open_write(p, 0);
    assert(fd >= 0);
    inject(F_ZERO);
    errno = 0;
    rc = sc_write_all(fd, out, 100);
    int e = errno;
    calls = g_calls;
    inject(F_NONE);
    assert(rc == -1 && e == EIO);
    assert(calls == 1);

    /* The next call still works. */
    assert(sc_write_all(fd, "ok\n", 3) == 0);
    assert(sc_close(fd) == 0);
    disk = slurp(p, &len);
    assert(len == 3 && memcmp(disk, "ok\n", 3) == 0);
    free(disk);

    /* A failure after 3000 bytes: -1 with the errno of write(), and the
       3000 bytes stay written. */
    fd = sc_open_write(p, 0);
    assert(fd >= 0);
    inject(F_FAIL_AFTER);
    g_limit      = 3000;
    g_fail_errno = ENOSPC;
    errno        = 0;
    rc = sc_write_all(fd, out, 10000);
    e  = errno;
    inject(F_NONE);
    assert(rc == -1 && e == ENOSPC);
    assert(sc_close(fd) == 0);
    disk = slurp(p, &len);
    assert(len == 3000 && memcmp(disk, out, 3000) == 0);
    free(disk);

    /* Real errors of the kernel. */
    fd = sc_open_write("/dev/full", 0);
    assert(fd >= 0);
    errno = 0;
    assert(sc_write_all(fd, out, 10) == -1 && errno == ENOSPC);
    assert(sc_close(fd) == 0);

    int pfd[2];
    assert(pipe(pfd) == 0);
    assert(sc_close(pfd[0]) == 0);
    errno = 0;
    assert(sc_write_all(pfd[1], "x", 1) == -1 && errno == EPIPE);
    assert(sc_close(pfd[1]) == 0);

    fd = sc_open_write(p, 1);
    assert(fd >= 0);
    assert(sc_write_all(fd, "end\n", 4) == 0);
    assert(sc_close(fd) == 0);
    free(out);
}

/* ---- sc_read_full -------------------------------------------------------- */

static void test_read_full(void)
{
    char p[128];
    path_in(p, sizeof p, "read.bin");
    char *out = pattern(7);
    char *in  = malloc(BIG);
    assert(in != NULL);
    put(p, out, BIG);

    /* Short reads and EINTR: n bytes, each in its place. */
    int fd = sc_open_read(p);
    assert(fd >= 0);
    inject(F_SHORT_EINTR);
    ssize_t n     = sc_read_full(fd, in, BIG);
    int     calls = g_calls;
    char    tail[16];
    ssize_t end   = sc_read_full(fd, tail, sizeof tail);
    inject(F_NONE);
    assert(n == BIG);
    assert(calls > BIG / 777);
    assert(memcmp(in, out, BIG) == 0);
    assert(end == 0);
    assert(sc_close(fd) == 0);

    /* Fewer bytes only at the end of the input. */
    put(p, "0123456789", 10);
    fd = sc_open_read(p);
    assert(fd >= 0);
    assert(sc_read_full(fd, in, 100) == 10 && memcmp(in, "0123456789", 10) == 0);
    assert(sc_read_full(fd, in, 100) == 0);
    assert(sc_close(fd) == 0);

    /* n == 0 reads nothing, whatever fd is. */
    inject(F_SHORT_EINTR);
    assert(sc_read_full(-1, in, 0) == 0);
    assert(sc_read_full(-1, NULL, 0) == 0);
    assert(g_calls == 0);
    inject(F_NONE);

    errno = 0;
    assert(sc_read_full(-1, in, 4) == -1 && errno == EBADF);

    /* A failure after 3000 bytes: -1 with the errno of read(). The bytes
       are in buf, but not counted. */
    put(p, out, BIG);
    fd = sc_open_read(p);
    assert(fd >= 0);
    memset(in, 0, BIG);
    inject(F_FAIL_AFTER);
    g_limit      = 3000;
    g_fail_errno = EIO;
    errno        = 0;
    n            = sc_read_full(fd, in, 10000);
    int e        = errno;
    inject(F_NONE);
    assert(n == -1 && e == EIO);
    assert(memcmp(in, out, 3000) == 0);

    /* The next call still works, from where the reads stopped. */
    assert(sc_read_full(fd, in, 1000) == 1000 && memcmp(in, out + 3000, 1000) == 0);
    assert(sc_close(fd) == 0);

    /* Real errors of the kernel. */
    fd = open(g_dir, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    errno = 0;
    assert(sc_read_full(fd, in, 10) == -1 && errno == EISDIR);
    assert(sc_close(fd) == 0);

    fd = sc_open_write(p, 1);
    assert(fd >= 0);
    errno = 0;
    assert(sc_read_full(fd, in, 10) == -1 && errno == EBADF);
    assert(sc_close(fd) == 0);

    free(in);
    free(out);
}

/* A pipe holds 64 kB, so 1 MB through it gives short reads, and a write that
   waits for the reader. A SIGALRM each millisecond, in both processes,
   interrupts the calls that wait: read() fails with EINTR, and write()
   returns the part that it wrote before the signal. */
static void test_pipe_with_signals(void)
{
    char *out = pattern(13);
    char *in  = malloc(BIG);
    assert(in != NULL);
    catch_alarm();

    int pfd[2];
    assert(pipe(pfd) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(pfd[0]);
        g_alarms = 0;
        alarm_every_us(1000, 1000);
        int rc = sc_write_all(pfd[1], out, BIG);
        alarm_every_us(0, 0);
        /* Under valgrind, memory that is not freed at _exit() is an error,
           and it changes the exit status of the child. */
        free(in);
        free(out);
        _exit(rc == 0 ? 0 : 1);
    }
    assert(sc_close(pfd[1]) == 0);

    g_alarms = 0;
    alarm_every_us(1000, 1000);
    sleep_ms(20);               /* let the pipe fill, so the writer waits */
    ssize_t n = sc_read_full(pfd[0], in, BIG);
    char    extra;
    ssize_t end = sc_read_full(pfd[0], &extra, 1);
    alarm_every_us(0, 0);

    assert(n == BIG);
    assert(memcmp(in, out, BIG) == 0);
    assert(end == 0);
    assert(g_alarms > 0);
    assert(sc_close(pfd[0]) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    free(in);
    free(out);
}

/* ---- sc_close ------------------------------------------------------------ */

static void test_close(void)
{
    char p[128];
    path_in(p, sizeof p, "close.txt");

    int fd = sc_open_write(p, 0);
    assert(fd >= 0);
    assert(sc_close(fd) == 0);
    assert(!fd_is_open(fd));

    errno = 0;
    assert(sc_close(fd) == -1 && errno == EBADF);
    errno = 0;
    assert(sc_close(-1) == -1 && errno == EBADF);

    /* EINTR: the descriptor is released already. sc_close returns 0, and
       does not call close() a second time. */
    fd = sc_open_write(p, 0);
    assert(fd >= 0);
    inject(F_CLOSE_EINTR);
    int rc    = sc_close(fd);
    int calls = g_calls;
    inject(F_NONE);
    assert(rc == 0);
    assert(calls == 1);
    assert(!fd_is_open(fd));

    fd = sc_open_read(p);
    assert(fd >= 0);
    assert(sc_close(fd) == 0);
}

static void remove_dir(void)
{
    static const char *const names[] = {
        "mode.txt", "append.txt", "two.txt", "read.txt", "after-error.txt",
        "fifo", "write.bin", "read.bin", "close.txt",
    };
    char p[128];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        path_in(p, sizeof p, names[i]);
        unlink(p);
    }
    assert(rmdir(g_dir) == 0);
}

int main(void)
{
    /* A write to a pipe with no reader must fail with EPIPE, not end the
       test. */
    signal(SIGPIPE, SIG_IGN);

    snprintf(g_dir, sizeof g_dir, "/tmp/sentinel_test_io_XXXXXX");
    assert(mkdtemp(g_dir) != NULL);

    test_open_write_modes();
    test_append_truncate();
    test_append_two_writers();
    test_open_read();
    test_open_write_errors();
    test_open_eintr();
    test_write_all();
    test_read_full();
    test_pipe_with_signals();
    test_close();

    remove_dir();
    printf("test_io: each assertion passes\n");
    return 0;
}
