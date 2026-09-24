/*
 * service.c - launching, stopping and locating managed services.
 *
 * The window between fork() and exec() is the only point at which the
 * supervisor controls the child's execution environment. Three things are
 * configured there: the signal disposition is reset, stdout/stderr are
 * redirected into the service's log file with dup2(), and an address-space
 * limit is applied with setrlimit().
 */

#define _POSIX_C_SOURCE 200809L

#include "warden.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

const char *state_name(enum svc_state st)
{
    switch (st) {
    case S_STOPPED: return "stopped";
    case S_RUNNING: return "running";
    case S_BACKOFF: return "backoff";
    case S_FAILED:  return "failed";
    }
    return "?";
}

const char *policy_name(enum policy p)
{
    switch (p) {
    case P_ALWAYS:    return "always";
    case P_ONFAILURE: return "onfailure";
    case P_NEVER:     return "never";
    }
    return "?";
}

struct service *service_find(const char *name)
{
    for (int i = 0; i < service_count; i++)
        if (strcmp(services[i].name, name) == 0)
            return &services[i];
    return NULL;
}

struct service *service_by_pid(pid_t pid)
{
    for (int i = 0; i < service_count; i++)
        if (services[i].pid == pid)
            return &services[i];
    return NULL;
}

/*
 * Child-side setup. Everything here runs after fork() and before exec(),
 * in the child process only. Only async-signal-safe style work is done.
 */
static void child_setup(const struct service *s)
{
    int fd;

    /* Own process group, so the service does not receive terminal signals
     * aimed at the supervisor (job-control separation). */
    setpgid(0, 0);

    /* Default signal dispositions: handlers installed by the supervisor
     * are not meaningful in the service. */
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT,  SIG_DFL);

    /* Redirect stdout and stderr into the service log file. The descriptor
     * table is per-process kernel state that survives exec(). */
    fd = open(s->logfile, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }

    /* Address-space limit: allocations beyond this fail in the service
     * rather than pressuring the whole system. */
    if (s->mem_limit_mb > 0) {
        struct rlimit rl;
        rl.rlim_cur = (rlim_t)s->mem_limit_mb * 1024 * 1024;
        rl.rlim_max = rl.rlim_cur;
        setrlimit(RLIMIT_AS, &rl);
    }
}

/*
 * Create the service process. Caller must hold table_lock.
 * Returns 0 on success, -1 on failure.
 */
int service_start_locked(struct service *s)
{
    pid_t pid;

    pid = fork();

    if (pid < 0) {
        log_msg("[%s] fork failed: %s", s->name, strerror(errno));
        s->state = S_FAILED;
        return -1;
    }

    if (pid == 0) {
        child_setup(s);
        execvp(s->argv[0], s->argv);
        /* Only reached if exec failed. */
        fprintf(stderr, "exec '%s' failed: %s\n",
                s->argv[0], strerror(errno));
        _exit(127);
    }

    s->pid         = pid;
    s->state       = S_RUNNING;
    s->manual_stop = 0;
    s->started_at  = time(NULL);
    s->last_ticks  = 0;
    s->last_sample = 0;
    s->cpu_pct     = 0.0;

    log_msg("[%s] started pid=%d (%s)", s->name, (int)pid, s->argv[0]);
    return 0;
}

/*
 * Start a service, bounding the number of spawn operations that may run
 * concurrently. Lock ordering is semaphore -> table_lock, never the reverse.
 */
void service_request_start(struct service *s)
{
    sem_wait(&restart_slots);
    pthread_mutex_lock(&table_lock);
    service_start_locked(s);
    pthread_mutex_unlock(&table_lock);
    sem_post(&restart_slots);
}

/* Send a signal to a running service. Caller must hold table_lock. */
void service_stop(struct service *s, int sig)
{
    if (s->pid <= 0)
        return;

    if (kill(s->pid, sig) < 0)
        log_msg("[%s] kill(%d) failed: %s", s->name, sig, strerror(errno));
    else
        log_msg("[%s] sent signal %d to pid=%d", s->name, sig, (int)s->pid);
}
