#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include "<sentinel/syscall.h>"

int main(void)
{
    /* Test sc_now_ns */
    assert(sc_now_ns() > 0);

    /* Test sc_now_wall */
    int64_t sec;
    int32_t usec;

    sc_now_wall(&sec, &usec);

    assert(sec > 0);
    assert(usec >= 0 && usec < 1000000);

    /* Test sc_stamp */
    char buf[32];

    assert(sc_stamp(buf, sizeof(buf)) > 0);

    /* Test errors */
    assert(sc_stamp(buf, 5) == -1);
    assert(errno == ERANGE);

    assert(sc_stamp(NULL, sizeof(buf)) == -1);
    assert(errno == EINVAL);

    printf("All tests passed.\n");
    printf("Current time: %lld.%06d\n", (long long)sec, usec);
    printf("Current timestamp: %s\n", buf);

    return 0;
}
