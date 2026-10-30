#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "<sentinel/syscall.h>"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>

int proc_report(pid_t pid, proc_info_t *out)
{
    if (out == NULL) {errno = EINVAL; return -1;}
        int fd;
        if (pid == 0) {pid = getpid();}

    char path[50];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    fd = sc_open_read(path);

    char buf[20000];
    ssize_t bytes = sc_read_full(fd, buf, sizeof(buf));
    int status = sc_close(fd);
    buf[bytes] = '\0';

    char *tok = strtok(buf, "\n");
    char *fields[100]; 

    int count = 0;

    while(tok != NULL && count < 100)
    {
        fields[count] = tok;
        count++;
        tok = strtok(NULL, "\n");
    }
    //put info into the struct
     out -> pid = pid;

    for (int i = 0; i < count; i++) 
    {
    if (strncmp(fields[i], "Name:", 5) == 0) {
    char *name = fields[i] + 5;
    while (*name == ' ' || *name == '\t') {name++;}
    snprintf(out->name, sizeof(out->name), "%s", name);
}
    else if (strncmp(fields[i], "PPid:", 5) == 0) {
        out->ppid = (pid_t)strtol(fields[i] + 5, NULL, 10);
    }
    else if (strncmp(fields[i], "Threads:", 8) == 0) {
        out->threads = strtol(fields[i] + 8, NULL, 10);
    }
    else if (strncmp(fields[i], "VmRSS:", 6) == 0) {
        out->vm_rss_kb = strtol(fields[i] + 6, NULL, 10);
    }
    else if (strncmp(fields[i], "VmSize:", 7) == 0) {
       out->vm_size_kb = strtol(fields[i] + 7, NULL, 10);
    }
    else if (strncmp(fields[i], "voluntary_ctxt_switches:", 24) == 0) {
        out->vol_ctxt = strtol(fields[i] + 24, NULL, 10);
    }
    else if (strncmp(fields[i], "nonvoluntary_ctxt_switches:", 27) == 0) {
        out->invol_ctxt = strtol(fields[i] + 27, NULL, 10);
    }
    }

        if (pid == 0) {
        snprintf(path, sizeof(path), "/proc/self/fd");
        } 
    
    else {
        snprintf(path, sizeof(path), "/proc/%d/fd", pid);
        }

    DIR = *dir = opendir(path);

    if(dir == NULL){
        return -1;
    }

//Dir goes here
}


int proc_report_write(int fd, const proc_info_t *info)
{
    if (info == NULL) {errno = EINVAL; return -1;}
    char buf[500];

   int len = snprintf(buf, sizeof(buf),
    "process report\n"
    "  pid        %d\n"
    "  ppid       %d\n"
    "  name       %s\n"
    "  vm_size    %ld kB\n"
    "  vm_rss     %ld kB\n"
    "  threads    %d\n"
    "  open_fds   %d\n"
    "  ctxt_vol   %ld\n"
    "  ctxt_invol %ld\n",info->pid, info->ppid, info->name, info->vm_size, info->vm_rss, info->threads, info->open_fds, info->ctxt_vol, info->ctxt_invol);

    if (len < 0 ) {errno = ERANGE; return -1;}
    if (sc_write_all(fd, buf, (size_t)len) < 0) {return -1;}
    return 0;
}