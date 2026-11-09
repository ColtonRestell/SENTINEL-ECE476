/* proc.c — what the kernel knows about a running process, from /proc.
 *
 * A program that cannot report its own state cannot show that it leaks
 * memory or descriptors. demo_lab1 counts its open descriptors before and
 * after it reads the feed, and a difference is a leak.
 */

#include "sentinel/syscall.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* /proc/<pid>/status is about 1.5 kB. The buffer grows when a kernel writes
   more, so that no field at the end is lost. */
#define STATUS_CHUNK 4096

/* Read the whole file at path into a new buffer, with a '\0' at its end.
   Returns NULL with errno on a failure. */
static char *read_text(const char *path)
{
    int fd = sc_open_read(path);
    if (fd < 0) {
        return NULL;
    }

    size_t cap  = STATUS_CHUNK;
    size_t used = 0;
    char  *text = malloc(cap + 1);
    if (text == NULL) {
        int saved = errno;
        sc_close(fd);
        errno = saved;
        return NULL;
    }

    for (;;) {
        ssize_t n = sc_read_full(fd, text + used, cap - used);
        if (n < 0) {
            int saved = errno;
            free(text);
            sc_close(fd);
            errno = saved;
            return NULL;
        }
        used += (size_t)n;
        if (used < cap) {
            break;              /* sc_read_full stops short only at the end */
        }
        char *bigger = realloc(text, cap * 2 + 1);
        if (bigger == NULL) {
            int saved = errno;
            free(text);
            sc_close(fd);
            errno = saved;
            return NULL;
        }
        text = bigger;
        cap *= 2;
    }
    text[used] = '\0';

    if (sc_close(fd) < 0) {
        int saved = errno;
        free(text);
        errno = saved;
        return NULL;
    }
    return text;
}

/* The value of the line "<key>:<spaces or tabs><value>", or NULL when the
   text has no such line. The whole key must match, up to the ':', because
   "voluntary_ctxt_switches" is also the end of "nonvoluntary_ctxt_switches". */
static const char *field(const char *text, const char *key)
{
    size_t klen = strlen(key);
    for (const char *line = text; *line != '\0';) {
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            const char *v = line + klen + 1;
            while (*v == ' ' || *v == '\t') {
                v++;
            }
            return v;
        }
        const char *nl = strchr(line, '\n');
        if (nl == NULL) {
            break;
        }
        line = nl + 1;
    }
    return NULL;
}

/* A field that /proc does not list is 0, as for VmSize of a kernel thread,
   which has no memory of its own. */
static long field_long(const char *text, const char *key)
{
    const char *v = field(text, key);
    return v == NULL ? 0 : strtol(v, NULL, 10);
}

/* The number of entries in the directory path, not counting "." and "..". */
static int count_entries(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL) {
        return -1;
    }

    int            n = 0;
    struct dirent *e;
    /* readdir() returns NULL at the end and on an error. Only an error
       changes errno. */
    errno = 0;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) {
            n++;
        }
    }
    if (errno != 0) {
        int saved = errno;
        closedir(d);
        errno = saved;
        return -1;
    }

    /* As in sc_close: EINTR from close() has released the descriptor. */
    if (closedir(d) < 0 && errno != EINTR) {
        return -1;
    }
    return n;
}

int proc_report(pid_t pid, proc_info_t *out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    pid_t self   = getpid();
    pid_t target = pid == 0 ? self : pid;
    char  path[64];

    snprintf(path, sizeof path, "/proc/%d/status", (int)target);
    char *status = read_text(path);
    if (status == NULL) {
        return -1;
    }

    memset(out, 0, sizeof *out);
    out->pid        = target;
    out->ppid       = (pid_t)field_long(status, "PPid");
    out->vm_rss_kb  = field_long(status, "VmRSS");
    out->vm_size_kb = field_long(status, "VmSize");
    out->threads    = (int)field_long(status, "Threads");
    out->vol_ctxt   = field_long(status, "voluntary_ctxt_switches");
    out->invol_ctxt = field_long(status, "nonvoluntary_ctxt_switches");

    const char *name = field(status, "Name");
    if (name != NULL) {
        size_t len = strcspn(name, "\n");
        if (len >= sizeof out->name) {
            len = sizeof out->name - 1;
        }
        memcpy(out->name, name, len);
        out->name[len] = '\0';
    }
    free(status);

    /* The status file is closed before the count, so its descriptor is not
       in the count. The descriptor of opendir() is, but only in the fd
       directory of the calling process. */
    snprintf(path, sizeof path, "/proc/%d/fd", (int)target);
    int fds = count_entries(path);
    if (fds < 0) {
        return -1;
    }
    out->open_fds = target == self ? fds - 1 : fds;
    return 0;
}

int proc_report_write(int fd, const proc_info_t *info)
{
    if (info == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* One buffer and one sc_write_all, so that the report does not mix with
       the output of a different writer on the same descriptor. */
    char buf[1024];
    int  n = snprintf(buf, sizeof buf,
                      "process report\n"
                      "  %-10s %d\n"
                      "  %-10s %d\n"
                      "  %-10s %.*s\n"
                      "  %-10s %ld kB\n"
                      "  %-10s %ld kB\n"
                      "  %-10s %d\n"
                      "  %-10s %d\n"
                      "  %-10s %ld\n"
                      "  %-10s %ld\n",
                      "pid", (int)info->pid,
                      "ppid", (int)info->ppid,
                      "name", (int)strnlen(info->name, sizeof info->name), info->name,
                      "vm_size", info->vm_size_kb,
                      "vm_rss", info->vm_rss_kb,
                      "threads", info->threads,
                      "open_fds", info->open_fds,
                      "ctxt_vol", info->vol_ctxt,
                      "ctxt_invol", info->invol_ctxt);
    if (n < 0 || (size_t)n >= sizeof buf) {
        /* Nine short lines and a name of at most 63 bytes fit, so this does
           not occur. */
        errno = EOVERFLOW;
        return -1;
    }
    return sc_write_all(fd, buf, (size_t)n);
}
