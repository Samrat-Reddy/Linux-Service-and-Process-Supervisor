/*
 * warden.h - shared declarations for the Warden service supervisor.
 */

#ifndef WARDEN_H
#define WARDEN_H

#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>

#define MAX_SERVICES    16
#define MAX_ARGS        16
#define MAX_LINE        512
#define MAX_NAME        32
#define MAX_PATH        256

#define RESTART_WINDOW  30   /* uptime that marks a service healthy again  */
#define MAX_BURST        5   /* consecutive failed restarts before giving up*/
#define BACKOFF_START    1   /* first retry delay, seconds                 */
#define BACKOFF_MAX     16   /* delay ceiling, seconds                     */
#define RESTART_SLOTS    2   /* concurrent spawn operations (semaphore)    */
#define HEALTH_INTERVAL  2   /* seconds between /proc samples              */
#define STOP_GRACE       5   /* seconds between SIGTERM and SIGKILL        */

enum svc_state { S_STOPPED, S_RUNNING, S_BACKOFF, S_FAILED };
enum policy    { P_ALWAYS, P_ONFAILURE, P_NEVER };

struct service {
    /* configuration */
    char           name[MAX_NAME];
    char           raw[MAX_LINE];        /* tokenised config line          */
    char          *argv[MAX_ARGS + 1];   /* points into raw[]              */
    enum policy    policy;
    long           mem_limit_mb;         /* 0 = no limit                   */
    char           logfile[MAX_PATH];

    /* runtime state */
    pid_t          pid;                  /* -1 when not running            */
    enum svc_state state;
    int            manual_stop;          /* stopped by administrator       */
    int            last_status;          /* raw wait(2) status word        */
    time_t         started_at;

    /* restart accounting */
    int            restarts;             /* total since supervisor start   */
    int            burst;                /* restarts in current window     */
    time_t         window_start;
    int            backoff;              /* current delay, seconds         */
    time_t         next_start;           /* earliest permitted restart     */

    /* health sample, from /proc/<pid>/stat */
    char           procstate;            /* R, S, D, Z, T                  */
    unsigned long  minflt, majflt;       /* page fault counters            */
    long           rss_kb;
    double         cpu_pct;
    unsigned long  last_ticks;
    time_t         last_sample;
};

extern struct service   services[MAX_SERVICES];
extern int              service_count;

extern pthread_mutex_t  table_lock;   /* guards services[] entirely        */
extern pthread_cond_t   restart_cv;   /* signalled when work may be due    */
extern sem_t            restart_slots;/* bounds concurrent spawns          */

extern volatile sig_atomic_t shutting_down;
extern int  sigpipe[2];               /* self-pipe fed by signal handler   */
extern int  foreground;

/* Portable millisecond sleep. usleep() was removed from POSIX.1-2008,
 * so nanosleep() is used instead. */
static inline void msleep(long ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* log.c */
int  log_open(const char *path, int echo);
void log_msg(const char *fmt, ...);
void log_close(void);

/* config.c */
int  config_load(const char *path);

/* service.c */
int         service_start_locked(struct service *s);
void        service_request_start(struct service *s);
void        service_stop(struct service *s, int sig);
struct service *service_find(const char *name);
struct service *service_by_pid(pid_t pid);
const char *state_name(enum svc_state st);
const char *policy_name(enum policy p);

/* supervisor.c */
int   signals_init(void);
void *reaper_thread(void *arg);
void  supervisor_run(void);
void  supervisor_shutdown(void);

/* health.c */
void *health_thread(void *arg);

/* control.c */
void *control_thread(void *arg);
extern char control_fifo[MAX_PATH];

#endif /* WARDEN_H */
