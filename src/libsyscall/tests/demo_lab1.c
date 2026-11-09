/* demo_lab1 — the demonstration program of Lab 1.
 *
 *   demo_lab1 <feed-file> [mission-log] [--follow IDLE_MS]
 *
 * It reads a feed of contact reports, and writes one line to the mission log
 * for each line of the feed. Section 3 of docs/lab1/lab1_brief.pdf is its
 * full specification, and the numbered steps below are the steps of Section
 * 3.2.
 *
 * The program does each step also after a fault that it can survive, and
 * keeps the exit status of the fault. A mission log that stops at the first
 * bad line loses each report after it, and the watch officer cannot tell
 * that from a quiet sensor.
 */

#include "sentinel/syscall.h"
#include "contact.h"
#include "linereader.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_LOG     "/tmp/sentinel_mission.log"
#define WAIT_MS         50
#define NS_PER_MS       1000000LL

#define EXIT_FAULT      1
#define EXIT_USAGE      2

static const char *g_prog = "demo_lab1";

/* The lines of the mission log that log_writef could not write. One message
   for each lost line would hide the cause on a full disk, where each line
   fails, so step 12 reports the count and the last reason one time. */
static long g_lost;
static int  g_lost_errno;

struct options {
    const char *feed;
    const char *log;
    long        idle_ms;        /* 0: no --follow */
};

struct counts {
    long read;
    long accepted;
    long rejected;
    long priority;
};

static void usage(void)
{
    fprintf(stderr, "usage: %s <feed-file> [mission-log] [--follow IDLE_MS]\n"
                    "       IDLE_MS is a whole number from 1 to %d\n",
            g_prog, INT_MAX);
}

/* A whole number from 1 to INT_MAX, in decimal digits only. strtol() would
   also take spaces, a sign and trailing text, and "--follow 3s" must not run
   as --follow 3. */
static long parse_idle(const char *s)
{
    if (*s == '\0') {
        return -1;
    }
    long v = 0;
    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        v = v * 10 + (*p - '0');
        if (v > INT_MAX) {
            return -1;
        }
    }
    return v >= 1 ? v : -1;
}

/* Returns 0, or -1 after it wrote the usage line. */
static int parse_args(int argc, char **argv, struct options *opt)
{
    opt->feed    = NULL;
    opt->log     = DEFAULT_LOG;
    opt->idle_ms = 0;

    int files = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--follow") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --follow needs a value\n", g_prog);
                usage();
                return -1;
            }
            long v = parse_idle(argv[++i]);
            if (v < 0) {
                fprintf(stderr, "%s: --follow %s: not a whole number from 1 to %d\n",
                        g_prog, argv[i], INT_MAX);
                usage();
                return -1;
            }
            opt->idle_ms = v;
            continue;
        }

        /* Each other argument is a file name, also "--follow=3000". */
        if (files == 0) {
            opt->feed = argv[i];
        } else if (files == 1) {
            opt->log = argv[i];
        } else {
            fprintf(stderr, "%s: too many file names: %s\n", g_prog, argv[i]);
            usage();
            return -1;
        }
        files++;
    }

    if (opt->feed == NULL) {
        fprintf(stderr, "%s: no feed file\n", g_prog);
        usage();
        return -1;
    }
    return 0;
}

/* Section 3.3: continue after a failed log_writef, and count the line. */
#define LOG_LINE(lg, level, ...)                                    \
    do {                                                            \
        if (log_writef((lg), (level), __VA_ARGS__) < 0) {           \
            g_lost++;                                               \
            g_lost_errno = errno;                                   \
        }                                                           \
    } while (0)

/* Step 12. Also on an early exit, so that a lost start line is reported. */
static void report_lost(void)
{
    if (g_lost > 0) {
        fprintf(stderr, "%s: %ld line(s) of the mission log were lost; the last reason: %s\n",
                g_prog, g_lost, strerror(g_lost_errno));
    }
}

/* Close the mission log on a path that ends the run early. */
static int stop_early(log_t *lg)
{
    if (log_close(lg) < 0) {
        fprintf(stderr, "%s: cannot close the mission log: %s\n", g_prog, strerror(errno));
    }
    report_lost();
    return EXIT_FAULT;
}

/* Returns 0, or -1 when the standard output failed. The summary goes there,
   so a failure is a fault of the run. Once the stream has failed, each later
   flush fails too, so the message is written one time only. */
static int flush_stdout(void)
{
    static int reported;
    if (fflush(stdout) == EOF || ferror(stdout)) {
        if (!reported) {
            fprintf(stderr, "%s: cannot write the standard output: %s\n", g_prog, strerror(errno));
            reported = 1;
        }
        return -1;
    }
    return 0;
}

/* Wait between two polls of a feed that still grows. The demo then sleeps,
   and does not use the processor, while it waits for a sensor. */
static void wait_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * NS_PER_MS };
    /* A signal can end the wait early, with EINTR. The read loop then polls
       a little sooner, and checks the idle time again, so no branch is
       necessary. EINVAL needs a value out of range, and t is in range. */
    (void)nanosleep(&t, NULL);
}

static void log_report(log_t *lg, long lineno, const char *line, size_t len, struct counts *c)
{
    contact_t     rec;
    contact_err_t err = contact_parse(line, len, &rec);

    if (err != CONTACT_OK) {
        c->rejected++;
        LOG_LINE(lg, LOG_INFO, "rejected line %ld: %s", lineno, contact_strerror(err));
    } else if (rec.flag == PRIORITY_PRIORITY) {
        c->accepted++;
        c->priority++;
        LOG_LINE(lg, LOG_PRIORITY, "priority contact %s from %s at %.4f,%.4f speed %.1f",
                 rec.id, rec.sensor, rec.lat, rec.lon, rec.speed);
    } else {
        c->accepted++;
        LOG_LINE(lg, LOG_INFO, "contact %s from %s", rec.id, rec.sensor);
    }
}

/* Step 7, the read loop of Figure 1. Returns 0, or -1 after a read error. */
static int read_feed(linereader_t *lr, log_t *lg, long idle_ms, int64_t start_ns, struct counts *c)
{
    /* Rule 2: the idle time starts at step 6, and again at each new line. */
    int64_t last_line_ns = start_ns;

    for (;;) {
        const char *line = NULL;
        size_t      len  = 0;
        int         r    = lr_next_line(lr, &line, &len);

        if (r == 1) {
            c->read++;
            last_line_ns = sc_now_ns();
            log_report(lg, c->read, line, len, c);
            continue;
        }
        if (r == 0) {
            return 0;
        }

        int err = errno;
        if (err == EMSGSIZE) {
            /* Rule 3: the reader dropped this one line and recovered. It is
               still a line of the feed, so it counts, and it is a new line
               for the idle time. */
            c->read++;
            c->rejected++;
            last_line_ns = sc_now_ns();
            LOG_LINE(lg, LOG_INFO, "rejected line %ld: %s", c->read,
                     contact_strerror(CONTACT_ERR_TOO_LONG));
            continue;
        }
        if (err == EAGAIN && idle_ms > 0) {
            /* Rule 1: EAGAIN is "nothing yet". Only the idle time ends the
               run. A wait does not start the idle time again. */
            if (sc_now_ns() - last_line_ns >= idle_ms * NS_PER_MS) {
                return 0;
            }
            wait_ms(WAIT_MS);
            continue;
        }

        LOG_LINE(lg, LOG_SYSTEM, "feed read failed: %s", strerror(err));
        return -1;
    }
}

int main(int argc, char **argv)
{
    struct options opt;
    if (parse_args(argc, argv, &opt) < 0) {
        return EXIT_USAGE;
    }

    int status = 0;

    /* 1 */
    log_t *lg = log_open(opt.log);
    if (lg == NULL) {
        fprintf(stderr, "%s: cannot open the mission log %s: %s\n", g_prog, opt.log, strerror(errno));
        return EXIT_FAULT;
    }

    /* 2 */
    LOG_LINE(lg, LOG_SYSTEM, "SENTINEL Part 1 starting, feed=%s", opt.feed);

    /* 3: the feed is not open yet, so this count holds only what stays open
       for the whole run. */
    proc_info_t before;
    if (proc_report(0, &before) < 0) {
        fprintf(stderr, "%s: cannot read /proc for this process: %s\n", g_prog, strerror(errno));
        return stop_early(lg);
    }
    LOG_LINE(lg, LOG_SYSTEM, "pid=%d ppid=%d name=%s open_fds=%d",
             (int)before.pid, (int)before.ppid, before.name, before.open_fds);

    /* 4 */
    int fd = sc_open_read(opt.feed);
    if (fd < 0) {
        fprintf(stderr, "%s: cannot open the feed %s: %s\n", g_prog, opt.feed, strerror(errno));
        return stop_early(lg);
    }

    /* 5: own_fd 1, so that lr_close closes the feed. bufsize 0 is the
       default of 4096 bytes. */
    linereader_t *lr = opt.idle_ms > 0 ? lr_open_follow(fd, 0, 1) : lr_open(fd, 0, 1);
    if (lr == NULL) {
        fprintf(stderr, "%s: cannot make a line reader for the feed: %s\n", g_prog, strerror(errno));
        if (sc_close(fd) < 0) {
            fprintf(stderr, "%s: cannot close the feed: %s\n", g_prog, strerror(errno));
        }
        return stop_early(lg);
    }

    /* 6, 7 and 8 */
    struct counts c       = { 0, 0, 0, 0 };
    int64_t       t_start = sc_now_ns();
    if (read_feed(lr, lg, opt.idle_ms, t_start, &c) < 0) {
        status = EXIT_FAULT;
    }
    int64_t t_stop     = sc_now_ns();
    double  elapsed_ms = (double)(t_stop - t_start) / (double)NS_PER_MS;

    /* 9: the reader closes the feed. The after count must then equal the
       before count, or a descriptor leaked. */
    lr_close(lr);
    proc_info_t after;
    int         have_after = proc_report(0, &after) == 0;
    if (!have_after) {
        fprintf(stderr, "%s: cannot read /proc for this process: %s\n", g_prog, strerror(errno));
        status = EXIT_FAULT;
    }

    /* 10 */
    printf("  lines read   %ld\n", c.read);
    printf("  accepted     %ld\n", c.accepted);
    printf("  rejected     %ld\n", c.rejected);
    printf("  priority     %ld\n", c.priority);
    printf("  elapsed      %.3f ms\n", elapsed_ms);
    if (have_after) {
        /* printf() keeps its output in a buffer, and proc_report_write
           writes to the descriptor directly. Without the flush, the report
           comes before the summary. */
        if (flush_stdout() < 0) {
            status = EXIT_FAULT;
        }
        if (proc_report_write(STDOUT_FILENO, &after) < 0) {
            fprintf(stderr, "%s: cannot write the process report: %s\n", g_prog, strerror(errno));
            status = EXIT_FAULT;
        }
        printf("  open_fds before=%d after=%d (%s)\n", before.open_fds, after.open_fds,
               before.open_fds == after.open_fds ? "balanced" : "LEAKED");
    }
    if (flush_stdout() < 0) {
        status = EXIT_FAULT;
    }

    /* 11 */
    LOG_LINE(lg, LOG_SYSTEM, "Part 1 complete: %ld read, %ld accepted, %ld rejected, %ld priority, %.3f ms",
             c.read, c.accepted, c.rejected, c.priority, elapsed_ms);
    if (log_close(lg) < 0) {
        fprintf(stderr, "%s: cannot close the mission log: %s\n", g_prog, strerror(errno));
        status = EXIT_FAULT;
    }

    /* 12 */
    if (g_lost > 0) {
        report_lost();
        status = EXIT_FAULT;
    }

    /* A feed with lines, and not one of them a report: the sensors or the
       file are wrong, and the run did not do its work. */
    if (c.read > 0 && c.accepted == 0) {
        status = EXIT_FAULT;
    }
    return status;
}
