/*
 * health.c - periodic sampling of kernel-maintained process state.
 *
 * Liveness is more than "the PID still exists": a service can be alive and
 * stuck, or alive and consuming memory without bound. The kernel already
 * tracks this information; /proc exposes it through the ordinary file API.
 *
 * /proc/<pid>/stat field numbers used here (proc(5)):
 *    3  state        R running, S sleeping, D uninterruptible, Z zombie
 *   10  minflt       minor faults (resolved without disk I/O)
 *   12  majflt       major faults (required a disk read)
 *   14  utime        user-mode CPU ticks
 *   15  stime        kernel-mode CPU ticks
 *   24  rss          resident pages
 */

#define _POSIX_C_SOURCE 200809L

#include "warden.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct sample {
    char          state;
    unsigned long minflt, majflt;
    unsigned long utime, stime;
    long          rss_pages;
};

static int read_proc_stat(pid_t pid, struct sample *out)
{
    char  path[64], buf[1024], *p;
    FILE *fp;
    size_t n;

    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    fp = fopen(path, "r");
    if (fp == NULL)
        return -1;                       /* process already gone */

    n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    if (n == 0)
        return -1;
    buf[n] = '\0';

    /* The command name is field 2 and may itself contain spaces and
     * parentheses, so parsing starts after the LAST ')'. */
    p = strrchr(buf, ')');
    if (p == NULL)
        return -1;
    p++;

    {
        char          st;
        int           ppid, pgrp, sess, tty, tpgid;
        unsigned      flags;
        unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
        long          cutime, cstime, prio, nice, nthreads, itreal;
        unsigned long long starttime;
        unsigned long vsize;
        long          rss;

        if (sscanf(p, " %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu "
                      "%ld %ld %ld %ld %ld %ld %llu %lu %ld",
                   &st, &ppid, &pgrp, &sess, &tty, &tpgid, &flags,
                   &minflt, &cminflt, &majflt, &cmajflt, &utime, &stime,
                   &cutime, &cstime, &prio, &nice, &nthreads, &itreal,
                   &starttime, &vsize, &rss) != 22)
            return -1;

        out->state     = st;
        out->minflt    = minflt;
        out->majflt    = majflt;
        out->utime     = utime;
        out->stime     = stime;
        out->rss_pages = rss;
    }

    return 0;
}

void *health_thread(void *arg)
{
    long ticks_per_sec = sysconf(_SC_CLK_TCK);
    long page_kb       = sysconf(_SC_PAGESIZE) / 1024;

    (void)arg;

    while (!shutting_down) {
        for (int slept = 0; slept < HEALTH_INTERVAL * 5 && !shutting_down;
             slept++)
            msleep(200);

        if (shutting_down)
            break;

        pthread_mutex_lock(&table_lock);

        for (int i = 0; i < service_count; i++) {
            struct service *s = &services[i];
            struct sample   sm;
            time_t          now = time(NULL);

            if (s->pid <= 0 || s->state != S_RUNNING)
                continue;

            if (read_proc_stat(s->pid, &sm) < 0)
                continue;                /* terminated between checks */

            /* CPU percentage from the tick delta since the last sample. */
            {
                unsigned long total = sm.utime + sm.stime;
                if (s->last_sample != 0 && now > s->last_sample) {
                    double secs  = (double)(now - s->last_sample);
                    double delta = (double)(total - s->last_ticks);
                    s->cpu_pct = 100.0 * delta / (double)ticks_per_sec / secs;
                }
                s->last_ticks  = total;
                s->last_sample = now;
            }

            s->procstate = sm.state;
            s->minflt    = sm.minflt;
            s->majflt    = sm.majflt;
            s->rss_kb    = sm.rss_pages * page_kb;

            if (sm.state == 'Z')
                log_msg("[%s] pid=%d is a zombie awaiting reaping",
                        s->name, (int)s->pid);
            else if (sm.state == 'D')
                log_msg("[%s] pid=%d blocked in uninterruptible sleep",
                        s->name, (int)s->pid);

            /* Two independent limits protect the system.
             *
             * setrlimit(RLIMIT_AS) is the kernel's hard cap, applied at
             * launch: allocations beyond it simply fail inside the service.
             *
             * This check is the supervisor's own soft cap, at 80% of the
             * same figure. Because it is based on resident memory sampled
             * from /proc, it acts on what the service is actually using and
             * can intervene before the hard limit is reached. */
            if (s->mem_limit_mb > 0 &&
                s->rss_kb > s->mem_limit_mb * 1024 * 8 / 10) {
                log_msg("[%s] RSS %ldkB exceeds soft limit (80%% of %ldMB), "
                        "terminating", s->name, s->rss_kb, s->mem_limit_mb);
                service_stop(s, SIGTERM);
            }
        }

        pthread_mutex_unlock(&table_lock);
    }

    return NULL;
}
