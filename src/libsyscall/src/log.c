#include <stddef.h>
#include <sys/stat.h>
#include "sentinel/syscall.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct log {int fd;};

log_t *log_open(const char *path)
{
    if(path == NULL) {errno = EINVAL; return NULL;}

        int fd = open(path, O_RDWR | O_APPEND | O_CREAT, 0600);

        while (fd < 0 && errno == EINTR)
        {fd = open(path, O_RDWR | O_APPEND | O_CREAT, 0600);}
    
        if(fd < 0) {return NULL;}

        int result = fchmod(fd, 0600);

        if(result == -1)
            { 
                int error = errno; 
                close(fd);
                errno = error; 
                return NULL; 
            }

        log_t *new_log = malloc(sizeof(*new_log)); 
            if (new_log == NULL) 
                {
                    int errorlog = errno; 
                    close(fd); 
                    errno = errorlog; 
                    return NULL;
                }
        new_log -> fd = fd; // (*fd).fd = fd
    return new_log; 
}

int log_writef(log_t *lg, log_level_t level, const char *fmt, ...)
{
    if(lg == NULL || fmt == NULL) {errno = EINVAL; return -1;}

    char buf[512];
        va_list args;
        va_start(args, fmt);
            int result = vsnprintf(buf,sizeof(buf),fmt,args);
        va_end(args);
                if(result == -1) {errno = EOVERFLOW; return -1;} //if vsnprintf fails
        
    const char* tempchar = log_level_name(level); 
            if(tempchar == NULL) {errno = EINVAL; return -1;}

        char buffer[SC_STAMP_MAX];
            int temp = sc_stamp(buffer, sizeof(buffer));
                if(temp == -1){return -1;}
        
        char tempbuf[512 - temp - strlen(tempchar)];
        int print = snprintf(tempbuf, sizeof(tempbuf), "%s %-8s %s\n", buffer, tempchar, buf);
            if(print == -1) {errno = EOVERFLOW; return -1;} //if snprintf fails

        for (int i = 0; buf[i] != '\0'; i++) 
        {
             if (buf[i] == '\n') {buf[i] = ' ';}
        }
        int write = sc_write_all(lg -> fd, tempbuf, (size_t)print); 
            if(write == -1){return -1;}
    return 0; 
}

int log_close(log_t *lg)
{
    if(lg == NULL) {return 0;}
        int result = sc_close(lg -> fd);
            free(lg); 
    return result; 
}

const char *log_level_name(log_level_t level)
{
    if (level == LOG_SYSTEM){return "SYSTEM";}
    if (level == LOG_INFO){return "INFO";}
    if (level == LOG_PENDING){return "PENDING";}
    if (level == LOG_PRIORITY){return "PRIORITY";}
    if (level == LOG_ALERT){return "ALERT";}
    if (level == LOG_TRACK){return "TRACK";}
    
    return NULL;
}