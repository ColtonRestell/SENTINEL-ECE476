/* test_clock.c — each promise and each error of clock.c.
 *
 * The test reads the two clocks itself with clock_gettime(), and compares
 * each function with the clock that syscall.h names for it.
 */

#undef NDEBUG               /* the asserts are the test */

#include "sentinel/syscall.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NS_PER_SEC 1000000000LL

static int64_t clock_ns(clockid_t id)
{
    struct timespec t;
    assert(clock_gettime(id, &t) == 0);
    return (int64_t)t.tv_sec * NS_PER_SEC + t.tv_nsec;
}

static void sleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) != 0 && errno == EINTR) {
    }
}

/* ---- sc_now_ns ----------------------------------------------------------- */

static void test_now_ns(void)
{
    /* The same clock as CLOCK_MONOTONIC. */
    int64_t mine = clock_ns(CLOCK_MONOTONIC);
    int64_t prev = sc_now_ns();
    int64_t diff = prev - mine;
    assert(diff >= 0 && diff < NS_PER_SEC);

    /* It never goes back. */
    for (int i = 0; i < 100000; i++) {
        int64_t now = sc_now_ns();
        assert(now >= prev);
        prev = now;
    }

    /* It measures a duration. */
    int64_t t0 = sc_now_ns();
    sleep_ms(100);
    int64_t ms = (sc_now_ns() - t0) / 1000000;
    assert(ms >= 100 && ms < 1000);
}

/* ---- sc_now_wall --------------------------------------------------------- */

static void test_now_wall(void)
{
    /* The same clock as CLOCK_REALTIME: a date. */
    int64_t real = clock_ns(CLOCK_REALTIME) / NS_PER_SEC;
    int64_t sec  = 0;
    int32_t usec = -1;
    sc_now_wall(&sec, &usec);
    assert(sec >= real && sec <= real + 2);
    assert(usec >= 0 && usec <= 999999);

    /* The value is between two readings of CLOCK_REALTIME. */
    for (int i = 0; i < 1000; i++) {
        int64_t before = clock_ns(CLOCK_REALTIME) / 1000;
        sc_now_wall(&sec, &usec);
        int64_t after = clock_ns(CLOCK_REALTIME) / 1000;
        int64_t got   = sec * 1000000 + usec;
        assert(usec >= 0 && usec <= 999999);
        assert(got >= before && got <= after);
    }

    /* A NULL pointer is allowed, and the other value is still written. */
    sec  = 0;
    usec = -1;
    sc_now_wall(&sec, NULL);
    assert(sec >= real);
    sc_now_wall(NULL, &usec);
    assert(usec >= 0 && usec <= 999999);
    sc_now_wall(NULL, NULL);
}

/* ---- sc_stamp ------------------------------------------------------------ */

/* 0 when s is "<digits>.<6 digits>", and the seconds in *sec. */
static int parse_stamp(const char *s, long long *sec, long *usec)
{
    char *end = NULL;
    if (*s < '0' || *s > '9') {
        return -1;
    }
    *sec = strtoll(s, &end, 10);
    if (*end != '.') {
        return -1;
    }
    const char *u = end + 1;
    for (int i = 0; i < 6; i++) {
        if (u[i] < '0' || u[i] > '9') {
            return -1;
        }
    }
    if (u[6] != '\0') {
        return -1;
    }
    *usec = strtol(u, NULL, 10);
    return 0;
}

static void test_stamp(void)
{
    char buf[SC_STAMP_MAX];
    memset(buf, '#', sizeof buf);

    int64_t before = clock_ns(CLOCK_REALTIME) / 1000;
    int     n      = sc_stamp(buf, sizeof buf);
    int64_t after  = clock_ns(CLOCK_REALTIME) / 1000;
    assert(n > 0 && (size_t)n < sizeof buf);
    assert((size_t)n == strlen(buf));

    long long sec;
    long      usec;
    assert(parse_stamp(buf, &sec, &usec) == 0);
    int64_t got = (int64_t)sec * 1000000 + usec;
    assert(got >= before && got <= after);

    /* A buffer of exactly the stamp and its '\0' is enough. */
    char exact[SC_STAMP_MAX];
    assert(sc_stamp(exact, (size_t)n + 1) == n);
    assert(strlen(exact) == (size_t)n);

    /* A buffer that is too small: ERANGE, and buf does not change. */
    char small[SC_STAMP_MAX];
    memset(small, '#', sizeof small);
    errno = 0;
    assert(sc_stamp(small, (size_t)n) == -1 && errno == ERANGE);
    errno = 0;
    assert(sc_stamp(small, 4) == -1 && errno == ERANGE);
    errno = 0;
    assert(sc_stamp(small, 0) == -1 && errno == ERANGE);
    for (size_t i = 0; i < sizeof small; i++) {
        assert(small[i] == '#');
    }

    errno = 0;
    assert(sc_stamp(NULL, sizeof buf) == -1 && errno == EINVAL);
    errno = 0;
    assert(sc_stamp(NULL, 0) == -1 && errno == EINVAL);

    /* The next call still works. */
    assert(sc_stamp(small, sizeof small) == n);
    assert(parse_stamp(small, &sec, &usec) == 0);
}

int main(void)
{
    test_now_ns();
    test_now_wall();
    test_stamp();
    printf("test_clock: each assertion passes\n");
    return 0;
}
