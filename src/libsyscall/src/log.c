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

       int fd = open(path, O_WRONLY | O_CLOEXEC | O_APPEND | O_CREAT, 0600);

        while (fd < 0 && errno == EINTR)
        {fd = open(path, O_WRONLY | O_CLOEXEC | O_APPEND | O_CREAT, 0600);}
    
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
        new_log -> fd = fd; 
    return new_log; 
}

int log_writef(log_t *lg, log_level_t level, const char *fmt, ...){
   
   if(lg == NULL || fmt == NULL){
    errno = EINVAL;
    return -1;
   }

    char ts[SC_STAMP_MAX]; 
    int tslen = sc_stamp(ts, sizeof(ts));
    if(tslen == -1){
        return -1;
    }
    const char *lvl = log_level_name(level); 
    if(lvl == NULL){
        errno = EINVAL;
        return -1;
    }
    char buf[513]; 
    int bytes = snprintf(buf, sizeof(buf), "%s %-8s ", ts, lvl);

    if (bytes < 0){
        errno = EOVERFLOW; 
        return -1;
    }
    
    size_t max_size = 512 - bytes -1;//remaining size for message, room for newline at the end

    char message[512]; 
    va_list args; 
    va_start(args, fmt);
    int msgbytes = vsnprintf(message, max_size + 1, fmt, args); //max size + 1 because we need room for the null terminator
    va_end(args);


    if(msgbytes < 0){
        errno = EOVERFLOW;
        return -1;
    }

    size_t max = (size_t)msgbytes;

    if(max > max_size){ //if the number of bytes written exceeds the size of the message, update max to max_size
        max = max_size;
    }

    for(size_t i = 0; i < max; i++){
        if(message[i] == '\n'){
            message[i] = ' '; 
        }
    }

    strcat(buf, message);
    strcat(buf, "\n");
    size_t write_amount = strlen(buf);

    int result = sc_write_all(lg->fd, buf, write_amount);

    if(result == -1){
        return -1;
    }

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