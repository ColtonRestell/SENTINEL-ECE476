#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include "sentinel/syscall.h"

int main(void)
{
    // Test opening a file
    log_t *lg = log_open("/home/ubuntu/sentinel-template/src/libsyscall/tests/test_calls/test.log");
    assert(lg != NULL);

    // Test writing a message
    assert(log_writef(lg, LOG_INFO, "Hello") == 0);

    // Test closing
    assert(log_close(lg) == 0);

    // Test opening with NULL
    assert(log_open(NULL) == NULL);

    // Test closing NULL
    assert(log_close(NULL) == 0);

    printf("All tests passed!\n");
    return 0;
}
