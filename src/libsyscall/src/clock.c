#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>
#include "sentinel/syscall.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define SC_STAMP_MAX 32

int64_t sc_now_ns(void)
{
    struct timespec tp; 
        clock_gettime(CLOCK_MONOTONIC, &tp);
        return (int64_t)tp.tv_sec * 1000000000LL + tp.tv_nsec;     
}

void sc_now_wall(int64_t *sec, int32_t *usec)
{
    struct timespec tp; 
        clock_gettime(CLOCK_REALTIME, &tp);
        if(sec != NULL)
            {*sec = (int64_t)tp.tv_sec;}
        if(usec != NULL)
            {*usec = (int32_t)(tp.tv_nsec / 1000);}
}

int sc_stamp(char *buf, size_t buflen)
{
    if(buf == NULL)
        {errno = EINVAL; return -1;}

    if(buflen == 0)
        {errno = ERANGE; return -1;}

    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);

    int len = snprintf(buf, buflen, "%lld.%06ld",
                       (long long)tp.tv_sec, tp.tv_nsec / 1000);

    if(len < 0)
        {return -1;}

    if((size_t)len >= buflen)
    {
        memset(buf, '#', buflen);
        errno = ERANGE;
        return -1;
    }

    return len;
}