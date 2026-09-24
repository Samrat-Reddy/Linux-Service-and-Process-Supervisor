/*
 * supervisor.c - asynchronous child reaping and restart policy.
 *
 * SIGCHLD can arrive at any instant, including while the service table is
 * being modified. A signal handler may only call async-signal-safe functions,
 * so it cannot lock a mutex or write to the log. The handler therefore does
 * one thing: write() a single byte into a pipe. The reaper thread reads that
 * pipe and performs the real work with ordinary locking. This is the
 * self-pipe technique.
 */

#define _POSIX_C_SOURCE 200809L

#include "warden.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

pthread_mutex_t table_lock    = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  restart_cv    = PTHREAD_COND_INITIALIZER;
sem_t           restart_slots;
volatile sig_atomic_t shutting_down;
int             sigpipe[2] = { -1, -1 };

/* ------------------------------------------------------- signal plumbing */

static void handler(int sig)
{
    int   saved = errno;          /* a handler must not disturb errno */
    char  byte  = (sig == SIGCHLD) ? 'C' : 'T';

    /* write() is async-signal-safe; the result is deliberately ignored. */
    if (write(sigpipe[1], &byte, 1) < 0) { /* nothing safe to do here */ }

    errno = saved;
}

int signals_init(void)
{
    struct sigaction sa;

    if (pipe(sigpipe) < 0) {
        perror("warden: pipe");
        return -1;
    }

    /* Non-blocking so the handler can never stall on a full pipe. */
    fcntl(sigpipe[0], F_SETFL, O_NONBLOCK);
    fcntl(sigpipe[1], F_SETFL, O_NONBLOCK);
    fcntl(sigpipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(sigpipe[1], F_SETFD, FD_CLOEXEC);

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);      /* block other signals inside the handler */
    sa.sa_flags   = SA_RESTART;   /* restart interrupted slow system calls  */

    if (sigaction(SIGCHLD, &sa, NULL) < 0 ||
        sigaction(SIGTERM, &sa, NULL) < 0 ||
        sigaction(SIGINT,  &sa, NULL) < 0) {
        perror("warden: sigaction");
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);     /* writing to a closed FIFO must not kill us */
    return 0;
}

/* ------------------------------------------------------- restart policy */

/*
 * Decide what happens to a service that has just terminated.
 * Caller must hold table_lock.
 */
static void apply_policy(struct service *s, int status)
{
    time_t now       = time(NULL);
    int    exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    long   uptime    = (long)(now - s->started_at);

    s->pid         = -1;
    s->last_status = status;
    s->procstate   = '-';
    s->cpu_pct     = 0.0;
    s->rss_kb      = 0;

    if (WIFEXITED(status))
        log_msg("[%s] exited status=%d after %lds",
                s->name, WEXITSTATUS(status), uptime);
    else if (WIFSIGNALED(status))
        log_msg("[%s] killed by signal %d (%s) after %lds",
                s->name, WTERMSIG(status), strsignal(WTERMSIG(status)),
                uptime);

    if (shutting_down || s->manual_stop) {
        s->state = S_STOPPED;
        return;
    }

    if (s->policy == P_NEVER || (s->policy == P_ONFAILURE && exited_ok)) {
        s->state = S_STOPPED;
        log_msg("[%s] not restarting (policy=%s)",
                s->name, policy_name(s->policy));
        return;
    }

    /* A service that stayed up for a full window is considered healthy
     * again, so its failure counter and backoff delay are reset. Otherwise
     * the counter accumulates across consecutive quick failures. */
    if (uptime >= RESTART_WINDOW) {
        s->burst   = 0;
        s->backoff = BACKOFF_START;
    }

    if (s->burst == 0)
        s->window_start = now;

    s->burst++;

    if (s->burst > MAX_BURST) {
        s->state = S_FAILED;
        log_msg("[%s] RESTART STORM: %d consecutive failures without "
                "staying up %ds, giving up", s->name, s->burst - 1,
                RESTART_WINDOW);
        return;
    }

    s->state      = S_BACKOFF;
    s->next_start = now + s->backoff;
    log_msg("[%s] restarting in %ds (attempt %d/%d)",
            s->name, s->backoff, s->burst, MAX_BURST);

    s->backoff *= 2;                       /* exponential backoff */
    if (s->backoff > BACKOFF_MAX)
        s->backoff = BACKOFF_MAX;

    s->restarts++;
}

/* ---------------------------------------------------------- reaper thread */

static int running_count(void)
{
    int n = 0;
    for (int i = 0; i < service_count; i++)
        if (services[i].pid > 0)
            n++;
    return n;
}

/*
 * Collects terminated children. waitpid() is called with WNOHANG in a loop
 * because several children may die between two signal deliveries, and
 * standard signals are not queued.
 */
static void reap_children(void)
{
    pid_t pid;
    int   status;

    for (;;) {
        pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0)
            break;                              /* children remain, none dead */
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            break;                              /* ECHILD: nothing left       */
        }

        pthread_mutex_lock(&table_lock);
        {
            struct service *s = service_by_pid(pid);
            if (s != NULL)
                apply_policy(s, status);
            else
                log_msg("reaped unknown child pid=%d", (int)pid);
        }
        pthread_cond_broadcast(&restart_cv);
        pthread_mutex_unlock(&table_lock);
    }
}

void *reaper_thread(void *arg)
{
    struct pollfd pfd = { .fd = sigpipe[0], .events = POLLIN };

    (void)arg;

    for (;;) {
        int n = poll(&pfd, 1, 200);

        if (n > 0) {
            char buf[64];
            ssize_t r;

            while ((r = read(sigpipe[0], buf, sizeof buf)) > 0) {
                for (ssize_t i = 0; i < r; i++) {
                    if (buf[i] == 'T' && !shutting_down) {
                        shutting_down = 1;
                        log_msg("shutdown requested");
                        pthread_mutex_lock(&table_lock);
                        pthread_cond_broadcast(&restart_cv);
                        pthread_mutex_unlock(&table_lock);
                    }
                }
            }
        }

        reap_children();

        if (shutting_down) {
            pthread_mutex_lock(&table_lock);
            int live = running_count();
            pthread_mutex_unlock(&table_lock);
            if (live == 0)
                break;
        }
    }

    return NULL;
}

/* ------------------------------------------------------ restart manager */

/*
 * Main thread. Sleeps on the condition variable until either a service
 * becomes due for restart or something changes in the table.
 */
void supervisor_run(void)
{
    while (!shutting_down) {
        struct service *due[MAX_SERVICES];
        int             ndue = 0;
        struct timespec ts;
        time_t          now;

        pthread_mutex_lock(&table_lock);

        now = time(NULL);
        for (int i = 0; i < service_count; i++) {
            struct service *s = &services[i];
            if (s->state == S_BACKOFF && s->next_start <= now)
                due[ndue++] = s;
        }

        if (ndue == 0) {
            ts.tv_sec  = now + 1;
            ts.tv_nsec = 0;
            pthread_cond_timedwait(&restart_cv, &table_lock, &ts);
            pthread_mutex_unlock(&table_lock);
            continue;
        }

        /* Mark them so no other thread picks the same service up. */
        for (int i = 0; i < ndue; i++)
            due[i]->state = S_STOPPED;

        pthread_mutex_unlock(&table_lock);

        /* Started outside the lock: service_request_start() takes the
         * semaphore first, and the ordering semaphore -> mutex must hold. */
        for (int i = 0; i < ndue; i++)
            service_request_start(due[i]);
    }
}

/* ------------------------------------------------------------- shutdown */

void supervisor_shutdown(void)
{
    log_msg("stopping all services");

    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < service_count; i++) {
        services[i].manual_stop = 1;
        if (services[i].pid > 0)
            service_stop(&services[i], SIGTERM);
    }
    pthread_mutex_unlock(&table_lock);

    /* Give services a chance to exit cleanly, then force them. */
    for (int waited = 0; waited < STOP_GRACE * 10; waited++) {
        pthread_mutex_lock(&table_lock);
        int live = running_count();
        pthread_mutex_unlock(&table_lock);
        if (live == 0)
            return;
        msleep(100);
    }

    pthread_mutex_lock(&table_lock);
    for (int i = 0; i < service_count; i++)
        if (services[i].pid > 0) {
            log_msg("[%s] did not stop, sending SIGKILL", services[i].name);
            service_stop(&services[i], SIGKILL);
        }
    pthread_mutex_unlock(&table_lock);
}
