#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/types.h>
#include "sentinel/syscall.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

int sc_open_read(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);

    while (fd < 0 && errno == EINTR)
        {fd = open(path, O_RDONLY | O_CLOEXEC);}

    return fd;   
}

int sc_open_write(const char *path, int append)
{
    int fd;
        if(append != 0)
            { fd = open(path, O_WRONLY | O_CLOEXEC | O_APPEND | O_CREAT, 0644);}
        else
            { fd = open(path, O_WRONLY |O_CLOEXEC | O_TRUNC | O_CREAT, 0644);}

        while (fd < 0 && errno == EINTR)
        {
        if(append != 0)
            { fd = open(path, O_WRONLY | O_CLOEXEC | O_APPEND | O_CREAT, 0644);}
        else
            { fd = open(path, O_WRONLY | O_CLOEXEC | O_TRUNC | O_CREAT, 0644);}
        }
    return fd; 
}

int sc_open_write_mode(const char *path, int append, mode_t mode)
{
    int fd;

    if (append != 0) {
        fd = open(path, O_WRONLY | O_CLOEXEC | O_APPEND | O_CREAT, mode);
    } else {
        fd = open(path, O_WRONLY| O_CLOEXEC | O_TRUNC | O_CREAT, mode);
    }

    while (fd < 0 && errno == EINTR) {
        if (append != 0) {
            fd = open(path, O_WRONLY | O_CLOEXEC | O_APPEND | O_CREAT, mode);
        } else {
            fd = open(path, O_WRONLY | O_CLOEXEC | O_TRUNC | O_CREAT, mode);
        }
    }

    return fd;
}

int sc_close(int fd)
{
    int temp = close(fd);

    if(temp < 0 && errno == EINTR)
        {return 0;}

    if(temp < 0)
        {return -1;}

    return 0;
}

int sc_write_all(int fd, const void *buf, size_t n)
{
    size_t written_bytes = 0; 
    size_t temp = 0;

        while (written_bytes < n)
        {
           temp = write(fd, (char *)buf+written_bytes, n-written_bytes);

            if((int)temp == -1 && errno == EINTR){}
            else if((int)temp == 0)
            {
                errno = EIO;
                return -1;
            }
            else if ((int)temp == -1)
                {return -1;}
            else 
                {written_bytes = written_bytes + temp;}
        }
    return 0;
}

ssize_t sc_read_full(int fd, void *buf, size_t n)
{
    size_t read_bytes = 0; 
    size_t temp = 0;

        while(read_bytes < n)
        {   
            temp = read(fd, (char *) buf+read_bytes, n-read_bytes);
            if((int)temp == 0)
                {return read_bytes;}
            else if((int)temp == -1 && errno == EINTR){}
            else if((int)temp == -1)
                {return -1;}
            else
                {read_bytes = read_bytes + temp;}
        }
    return read_bytes;
}