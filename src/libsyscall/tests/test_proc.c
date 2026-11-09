/* test_proc.c — each promise and each error of proc.c.
 *
 * The test reads /proc itself, and compares each field of proc_report with
 * the source that syscall.h names for it. A field that changes all the time,
 * such as a counter, must be between two readings that the test makes before
 * and after the call.
 */

#undef NDEBUG               /* the asserts are the test */

#include "sentinel/syscall.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* A pid above the largest that Linux gives (2^22), so no process has it. */
#define NO_SUCH_PID (4194304 + 1)

/* The number after "<key>:" in /proc/<pid>/status, or -1. */
static long status_of(pid_t pid, const char *key)
{
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    size_t klen = strlen(key);
    long   v    = -1;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            v = strtol(line + klen + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

static void comm_of(pid_t pid, char *buf, size_t n)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    assert(fgets(buf, (int)n, f) != NULL);
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
}

/* Entries in /proc/<pid>/fd. For this process, the descriptor of opendir is
   in the list, so it does not count. */
static int count_fds(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
    DIR *d = opendir(path);
    assert(d != NULL);
    int            n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            n++;
        }
    }
    closedir(d);
    return pid == getpid() ? n - 1 : n;
}

static void sleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

/* ---- proc_report of this process ---------------------------------------- */

static void test_self(void)
{
    proc_info_t info;
    memset(&info, 0x55, sizeof info);
    long vm_size_before = status_of(getpid(), "VmSize");
    assert(proc_report(0, &info) == 0);
    long vm_size_after = status_of(getpid(), "VmSize");

    char comm[64];
    comm_of(getpid(), comm, sizeof comm);

    assert(info.pid == getpid());
    assert(info.ppid == getppid());
    /* Under valgrind the name is the name of the tool, so the test compares
       it with comm and not with a fixed text. */
    assert(strcmp(info.name, comm) == 0);
    assert(info.threads == status_of(getpid(), "Threads"));
    assert(info.threads == 1);
    assert(info.vm_rss_kb > 0);
    assert(info.vm_size_kb >= info.vm_rss_kb);
    assert(info.vm_size_kb >= vm_size_before - 1024 && info.vm_size_kb <= vm_size_after + 1024);
    assert(info.vol_ctxt >= 0 && info.invol_ctxt >= 0);
    assert(info.open_fds == count_fds(getpid()));

    /* pid 0 and the pid of this process give the same process. */
    proc_info_t byid;
    assert(proc_report(getpid(), &byid) == 0);
    assert(byid.pid == info.pid);
    assert(byid.ppid == info.ppid);
    assert(strcmp(byid.name, info.name) == 0);
    assert(byid.open_fds == info.open_fds);
    assert(byid.threads == info.threads);
}

static void test_fds(void)
{
    proc_info_t before, during, after;
    assert(proc_report(0, &before) == 0);
    assert(before.open_fds == count_fds(getpid()));

    int a = open("/proc/self/status", O_RDONLY);
    int p[2];
    assert(a >= 0 && pipe(p) == 0);
    assert(proc_report(0, &during) == 0);
    assert(during.open_fds == before.open_fds + 3);

    close(a);
    close(p[0]);
    close(p[1]);
    assert(proc_report(0, &after) == 0);
    assert(after.open_fds == before.open_fds);
}

/* The values come from the status lines with the " kB" removed, so a block
   of 64 MB that the program writes shows in both. */
static void test_memory(void)
{
    enum { MB = 64 };
    proc_info_t before, after;
    assert(proc_report(0, &before) == 0);

    char *block = malloc((size_t)MB * 1024 * 1024);
    assert(block != NULL);
    /* A memset() before free() is a dead store, and the compiler removes
       it. A write through a volatile pointer stays, and each page that it
       touches becomes resident. */
    volatile char *touch = block;
    for (size_t i = 0; i < (size_t)MB * 1024 * 1024; i += 4096) {
        touch[i] = 1;
    }
    assert(proc_report(0, &after) == 0);

    assert(after.vm_size_kb - before.vm_size_kb >= (MB - 4) * 1024);
    assert(after.vm_rss_kb - before.vm_rss_kb >= (MB - 4) * 1024);
    assert(after.vm_rss_kb <= status_of(getpid(), "VmRSS") + 1024);
    free(block);
}

/* Each key is matched whole. "voluntary_ctxt_switches" is also the end of
   "nonvoluntary_ctxt_switches", so a search that is not whole can give one
   counter in place of the other. */
static void test_ctxt(void)
{
    proc_info_t before, after;
    assert(proc_report(0, &before) == 0);

    /* A sleep is a voluntary switch: the process asks to wait. */
    for (int i = 0; i < 20; i++) {
        sleep_ms(2);
    }

    long vol_lo   = status_of(getpid(), "voluntary_ctxt_switches");
    long invol_lo = status_of(getpid(), "nonvoluntary_ctxt_switches");
    assert(proc_report(0, &after) == 0);
    long vol_hi   = status_of(getpid(), "voluntary_ctxt_switches");
    long invol_hi = status_of(getpid(), "nonvoluntary_ctxt_switches");

    assert(after.vol_ctxt >= before.vol_ctxt + 20);
    assert(after.invol_ctxt >= before.invol_ctxt);
    assert(after.vol_ctxt >= vol_lo && after.vol_ctxt <= vol_hi);
    assert(after.invol_ctxt >= invol_lo && after.invol_ctxt <= invol_hi);
}

/* ---- proc_report of a different process --------------------------------- */

static void test_child(void)
{
    /* The child opens two files, and stops itself, so its descriptors stay
       open while this process counts them. */
    fflush(NULL);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        int x = open("/proc/self/status", O_RDONLY);
        int y = open("/proc/self/status", O_RDONLY);
        if (x < 0 || y < 0) {
            _exit(1);
        }
        raise(SIGSTOP);
        _exit(0);
    }
    int status = 0;
    assert(waitpid(child, &status, WUNTRACED) == child && WIFSTOPPED(status));

    proc_info_t mine, info;
    assert(proc_report(0, &mine) == 0);
    int expect = count_fds(child);
    assert(proc_report(child, &info) == 0);

    assert(info.pid == child);
    assert(info.ppid == getpid());
    assert(strcmp(info.name, mine.name) == 0);          /* fork() keeps the name */
    assert(info.threads == 1);
    /* Only for the calling process does proc_report remove its own
       descriptor from the count. The count is not compared with the count
       of this process: under valgrind, the child does not have the same
       descriptors of the tool as its parent. */
    assert(info.open_fds == expect);
    assert(info.open_fds >= 5);         /* 0, 1, 2 and the two opens */
    assert(info.vm_rss_kb > 0);

    kill(child, SIGKILL);
    assert(waitpid(child, NULL, 0) == child);

    /* The process is gone, so its pid gives ENOENT now. */
    errno = 0;
    assert(proc_report(child, &info) == -1 && errno == ENOENT);
}

/* A zombie has no memory and no descriptors, so /proc lists no Vm field for
   it. The kernel also gives its fd directory to root, as for a kernel
   thread. So for a user, proc_report fails with EACCES, as for a process of
   another user. For root, it works, and each field that /proc does not list
   is 0. waitid() with WNOWAIT waits until the child has ended, and leaves it
   a zombie. */
static void test_zombie(void)
{
    fflush(NULL);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        _exit(0);
    }
    siginfo_t si;
    assert(waitid(P_PID, (id_t)child, &si, WEXITED | WNOWAIT) == 0);
    assert(status_of(child, "VmSize") == -1);

    proc_info_t info;
    memset(&info, 0x55, sizeof info);
    errno = 0;
    int rc = proc_report(child, &info);
    if (geteuid() != 0) {
        assert(rc == -1 && errno == EACCES);
    } else {
        assert(rc == 0);
        assert(info.pid == child);
        assert(info.ppid == getpid());
        assert(info.vm_size_kb == 0);
        assert(info.vm_rss_kb == 0);
        assert(info.open_fds == 0);
    }
    assert(waitpid(child, NULL, 0) == child);

    /* The next call still works. */
    assert(proc_report(0, &info) == 0 && info.pid == getpid());
}

/* ---- the errors of proc_report ------------------------------------------ */

static void test_errors(void)
{
    proc_info_t info;

    errno = 0;
    assert(proc_report(0, NULL) == -1 && errno == EINVAL);
    assert(proc_report(0, &info) == 0 && info.pid == getpid());

    errno = 0;
    assert(proc_report(NO_SUCH_PID, &info) == -1 && errno == ENOENT);
    assert(proc_report(0, &info) == 0 && info.pid == getpid());

    /* A process of another user: its fd directory cannot be read. pid 1 is
       of another user on most machines, but not in each container, and root
       can read each process. */
    struct stat st;
    if (geteuid() != 0 && stat("/proc/1", &st) == 0 && st.st_uid != geteuid()) {
        errno = 0;
        assert(proc_report(1, &info) == -1 && errno == EACCES);
        assert(proc_report(0, &info) == 0 && info.pid == getpid());
    }
}

/* ---- proc_report_write -------------------------------------------------- */

static char *read_all(int fd, size_t *len)
{
    size_t cap = 4096, used = 0;
    char  *buf = malloc(cap);
    assert(buf != NULL);
    for (;;) {
        ssize_t n = read(fd, buf + used, cap - used - 1);
        assert(n >= 0);
        if (n == 0) {
            break;
        }
        used += (size_t)n;
        assert(used + 1 < cap);
    }
    buf[used] = '\0';
    *len      = used;
    return buf;
}

static void test_write(void)
{
    proc_info_t info;
    memset(&info, 0, sizeof info);
    info.pid        = 4321;
    info.ppid       = 1234;
    snprintf(info.name, sizeof info.name, "demo_lab1");
    info.vm_size_kb = 2600;
    info.vm_rss_kb  = 1100;
    info.threads    = 1;
    info.open_fds   = 4;
    info.vol_ctxt   = 87;
    info.invol_ctxt = 12;

    static const char want[] =
        "process report\n"
        "  pid        4321\n"
        "  ppid       1234\n"
        "  name       demo_lab1\n"
        "  vm_size    2600 kB\n"
        "  vm_rss     1100 kB\n"
        "  threads    1\n"
        "  open_fds   4\n"
        "  ctxt_vol   87\n"
        "  ctxt_invol 12\n";

    int p[2];
    assert(pipe(p) == 0);
    assert(proc_report_write(p[1], &info) == 0);
    close(p[1]);
    size_t len;
    char  *got = read_all(p[0], &len);
    close(p[0]);
    assert(strcmp(got, want) == 0);
    free(got);

    /* The longest name, and values with more digits than the field. */
    memset(info.name, 'n', sizeof info.name - 1);
    info.name[sizeof info.name - 1] = '\0';
    info.vm_size_kb = 123456789012L;
    info.invol_ctxt = -1;
    assert(pipe(p) == 0);
    assert(proc_report_write(p[1], &info) == 0);
    close(p[1]);
    got = read_all(p[0], &len);
    close(p[0]);
    char line[128];
    snprintf(line, sizeof line, "  name       %s\n", info.name);
    assert(strstr(got, line) != NULL);
    assert(strstr(got, "  vm_size    123456789012 kB\n") != NULL);
    assert(strstr(got, "  ctxt_invol -1\n") != NULL);
    free(got);

    /* Errors, and the next call still works. */
    errno = 0;
    assert(proc_report_write(1, NULL) == -1 && errno == EINVAL);
    errno = 0;
    assert(proc_report_write(-1, &info) == -1 && errno == EBADF);
    int full = open("/dev/full", O_WRONLY);
    assert(full >= 0);
    errno = 0;
    assert(proc_report_write(full, &info) == -1 && errno == ENOSPC);
    close(full);

    /* A report of this process, written and read back. */
    assert(proc_report(0, &info) == 0);
    assert(pipe(p) == 0);
    assert(proc_report_write(p[1], &info) == 0);
    close(p[1]);
    got = read_all(p[0], &len);
    close(p[0]);
    snprintf(line, sizeof line, "  pid        %d\n", (int)getpid());
    assert(strncmp(got, "process report\n", 15) == 0);
    assert(strstr(got, line) != NULL);
    free(got);
}

int main(void)
{
    test_self();
    test_fds();
    test_memory();
    test_ctxt();
    test_child();
    test_zombie();
    test_errors();
    test_write();
    printf("test_proc: each assertion passes\n");
    return 0;
}
