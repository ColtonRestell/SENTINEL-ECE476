#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include "sentinel/syscall.h"

int main(void)
{
    int fd;
    char buf[6];
    const char *path = "/home/ubuntu/sentinel-template/src/libsyscall/tests/test_calls/io_test.txt";

    /* Open file for writing */
    fd = sc_open_write(path, 0);
    assert(fd >= 0);

    /* Write hello */
    assert(sc_write_all(fd, "hello", 5) == 0);
    assert(sc_close(fd) == 0);

    /* Open file for reading */
    fd = sc_open_read(path);
    assert(fd >= 0);

    /* Read hello */
    assert(sc_read_full(fd, buf, 5) == 5);
    buf[5] = '\0';
    assert(strcmp(buf, "hello") == 0);
    assert(sc_close(fd) == 0);

    /* Test nonexistent file */
    assert(sc_open_read("/home/ubuntu/sentinel-template/src/libsyscall/tests/test_calls/no_such_file.txt") == -1);

    printf("All tests passed.\n");
    return 0;
}
