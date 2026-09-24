/*
 * warden.c - Linux Service and Process Supervisor
 *
 * Starts the services listed in a configuration file, detects when one dies,
 * reports why it died, and restarts it if its policy says so. An interactive
 * menu lets the administrator check status and start or stop services while
 * the supervisor is running.
 *
 * OS concepts demonstrated:
 *   fork(), execvp()        creating processes and loading programs
 *   waitpid()               collecting exit status, preventing zombies
 *   WIFEXITED/WIFSIGNALED   distinguishing normal exit from signal death
 *   signal handling         SIGCHLD notifies us that a child has terminated
 *   kill()                  stopping services
 *   /proc                   reading kernel-maintained process information
 *   file I/O                configuration file and activity log
 *
 * Build: make
 * Run:   ./warden services.conf
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_SERVICES  8
#define MAX_ARGS      8
#define MAX_LINE      256
#define MAX_NAME      32

#define RESTART_DELAY 2    /* seconds to wait before restarting        */
#define MAX_RETRIES   5    /* consecutive failures before giving up    */

struct service {
    char   name[MAX_NAME];
    char   raw[MAX_LINE];        /* tokenised config line              */
    char  *argv[MAX_ARGS + 1];   /* points into raw[]                  */
    int    restart;              /* 1 = restart automatically          */

    pid_t  pid;                  /* -1 when not running                */
    int    stopped_by_user;      /* do not auto-restart if set         */
    int    failures;             /* consecutive failed restarts        */
    int    gave_up;              /* too many failures, stop trying     */
    time_t restart_at;           /* 0 = nothing scheduled              */
    int    paused;               /* suspended with SIGSTOP             */
};

static struct service services[MAX_SERVICES];
static int   service_count;
static FILE *logfp;

/* Set by the SIGCHLD handler; checked by the main loop. Only a variable of
 * this type may be safely modified inside a signal handler. */
static volatile sig_atomic_t child_died;

/* ---------------------------------------------------------------- logging */

static void logmsg(const char *fmt, ...)
{
    char      stamp[16];
    time_t    now = time(NULL);
    struct tm tm;
    va_list   ap;

    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

    printf("%s  ", stamp);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);

    if (logfp != NULL) {
        fprintf(logfp, "%s  ", stamp);
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        va_end(ap);
        fputc('\n', logfp);
        fflush(logfp);
    }
}

/* ----------------------------------------------------------- config file */

/*
 * Each line is:   <name>  <yes|no>  <command>  [arguments...]
 * The second field says whether the service should be restarted when it dies.
 */
static int load_config(const char *path)
{
    FILE *fp;
    char  line[MAX_LINE];
    int   lineno = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "warden: cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof line, fp) != NULL) {
        struct service *s = &services[service_count];
        char *tok, *save, *hash;
        int   argc = 0;

        lineno++;

        hash = strchr(line, '#');           /* strip comments */
        if (hash != NULL)
            *hash = '\0';
        if (strspn(line, " \t\n") == strlen(line))
            continue;                       /* blank line */

        if (service_count >= MAX_SERVICES) {
            fprintf(stderr, "warden: too many services, stopping at line %d\n",
                    lineno);
            break;
        }

        memset(s, 0, sizeof *s);
        strncpy(s->raw, line, MAX_LINE - 1);

        tok = strtok_r(s->raw, " \t\n", &save);          /* name */
        if (tok == NULL)
            continue;
        strncpy(s->name, tok, MAX_NAME - 1);

        tok = strtok_r(NULL, " \t\n", &save);            /* restart flag */
        if (tok == NULL) {
            fprintf(stderr, "warden: line %d is incomplete, skipped\n", lineno);
            continue;
        }
        s->restart = (strcmp(tok, "yes") == 0);

        while ((tok = strtok_r(NULL, " \t\n", &save)) != NULL) {
            if (argc >= MAX_ARGS)
                break;
            s->argv[argc++] = tok;                       /* command + args */
        }
        if (argc == 0) {
            fprintf(stderr, "warden: line %d has no command, skipped\n", lineno);
            continue;
        }
        s->argv[argc] = NULL;

        s->pid = -1;
        service_count++;
    }

    fclose(fp);
    return service_count;
}

/* --------------------------------------------------------------- process */

/*
 * Create a child process and replace its program image with the service.
 * fork() duplicates the supervisor; execvp() then overwrites the child's
 * program while keeping the same PID.
 */
static void start_service(struct service *s)
{
    pid_t pid = fork();

    if (pid < 0) {
        logmsg("[%s] fork failed: %s", s->name, strerror(errno));
        return;
    }

    if (pid == 0) {                         /* child */
        execvp(s->argv[0], s->argv);
        /* Only reached if exec failed. _exit() is used instead of exit()
         * so the parent's buffered output is not flushed twice. */
        fprintf(stderr, "cannot run '%s': %s\n", s->argv[0], strerror(errno));
        _exit(127);
    }

    s->pid             = pid;               /* parent */
    s->stopped_by_user = 0;
    s->restart_at      = 0;
    s->paused          = 0;
    logmsg("[%s] started, pid %d", s->name, (int)pid);
}

/* Collect every child that has terminated and decide what to do next. */
static void reap_children(void)
{
    int   status;
    pid_t pid;

    /* WNOHANG returns immediately if no child has died. The loop is needed
     * because several children may terminate before we get here, and the
     * kernel does not queue one SIGCHLD per child. */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        struct service *s = NULL;

        for (int i = 0; i < service_count; i++)
            if (services[i].pid == pid)
                s = &services[i];

        if (s == NULL)
            continue;

        s->pid = -1;

        /* The status word returned by waitpid() is encoded, so it must be
         * examined with these macros rather than read as a plain number. */
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            logmsg("[%s] exited with status %d%s", s->name, code,
                   code == 0 ? " (normal)" : " (failure)");
            if (code == 0)
                s->failures = 0;
            else
                s->failures++;
        } else if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            logmsg("[%s] killed by signal %d (%s)",
                   s->name, sig, strsignal(sig));
            s->failures++;
        }

        if (s->stopped_by_user || !s->restart)
            continue;

        if (s->failures >= MAX_RETRIES) {
            if (!s->gave_up) {
                logmsg("[%s] failed %d times in a row, not restarting again",
                       s->name, s->failures);
                s->gave_up = 1;
            }
            continue;
        }

        s->restart_at = time(NULL) + RESTART_DELAY;
        logmsg("[%s] will restart in %d seconds", s->name, RESTART_DELAY);
    }
}

/* Restart any service whose waiting period has elapsed. */
static void do_pending_restarts(void)
{
    time_t now = time(NULL);

    for (int i = 0; i < service_count; i++) {
        struct service *s = &services[i];
        if (s->pid < 0 && s->restart_at != 0 && s->restart_at <= now)
            start_service(s);
    }
}

/* ------------------------------------------------------------- /proc info */

/*
 * Read the process state and resident memory of a running service.
 * The kernel exposes this through /proc as an ordinary readable file.
 */
static void read_proc_info(pid_t pid, char *state, long *rss_kb)
{
    char  path[64], buf[512], *p;
    FILE *fp;

    *state  = '?';
    *rss_kb = 0;

    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    fp = fopen(path, "r");
    if (fp == NULL)
        return;

    if (fgets(buf, sizeof buf, fp) != NULL) {
        /* The command name is in brackets and may contain spaces, so
         * parsing starts after the last ')'. */
        p = strrchr(buf, ')');
        if (p != NULL) {
            char st;
            long rss_pages;
            /* State is the next field; resident pages is the 22nd after it. */
            if (sscanf(p + 1, " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                              "%*u %*u %*d %*d %*d %*d %*d %*d %*u %*u %ld",
                       &st, &rss_pages) == 2) {
                *state  = st;
                *rss_kb = rss_pages * (sysconf(_SC_PAGESIZE) / 1024);
            }
        }
    }

    fclose(fp);
}

/* ------------------------------------------------------------------- menu */

static void show_status(void)
{
    printf("\n  %-12s %-10s %-8s %-8s %-10s\n",
           "SERVICE", "STATE", "PID", "MEM(kB)", "RESTART");
    printf("  ---------------------------------------------------\n");

    for (int i = 0; i < service_count; i++) {
        struct service *s = &services[i];
        char  procstate = '-';
        long  rss = 0;
        char  pidbuf[16] = "-";
        const char *state;

        if (s->pid > 0) {
            read_proc_info(s->pid, &procstate, &rss);
            snprintf(pidbuf, sizeof pidbuf, "%d", (int)s->pid);
            state = s->paused ? "paused" : "running";
        } else if (s->gave_up) {
            state = "failed";
        } else if (s->restart_at != 0) {
            state = "waiting";
        } else {
            state = "stopped";
        }

        printf("  %-12s %-10s %-8s %-8ld %-10s\n",
               s->name, state, pidbuf, rss, s->restart ? "yes" : "no");
    }
    printf("\n");
}

static void menu_stop(const char *name)
{
    for (int i = 0; i < service_count; i++) {
        struct service *s = &services[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if (s->pid < 0) {
            printf("  %s is not running\n", name);
            return;
        }
        s->stopped_by_user = 1;
        s->restart_at      = 0;
        /* A stopped process cannot act on SIGTERM until it is scheduled
         * again, so wake it first. */
        if (s->paused) {
            kill(s->pid, SIGCONT);
            s->paused = 0;
        }
        kill(s->pid, SIGTERM);              /* ask it to terminate */
        logmsg("[%s] sent SIGTERM to pid %d", s->name, (int)s->pid);
        return;
    }
    printf("  no service named '%s'\n", name);
}

static void menu_start(const char *name)
{
    for (int i = 0; i < service_count; i++) {
        struct service *s = &services[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if (s->pid > 0) {
            printf("  %s is already running\n", name);
            return;
        }
        s->failures = 0;
        s->gave_up  = 0;
        start_service(s);
        return;
    }
    printf("  no service named '%s'\n", name);
}

/*
 * SIGSTOP cannot be caught or ignored by the target process: the kernel
 * removes it from the run queue directly. SIGCONT makes it runnable again.
 * This is the same mechanism the shell uses for job control with Ctrl-Z.
 */
static void menu_pause(const char *name)
{
    for (int i = 0; i < service_count; i++) {
        struct service *s = &services[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if (s->pid < 0) {
            printf("  %s is not running\n", name);
            return;
        }
        if (s->paused) {
            printf("  %s is already paused\n", name);
            return;
        }
        kill(s->pid, SIGSTOP);              /* suspend it */
        s->paused = 1;
        logmsg("[%s] suspended with SIGSTOP (pid %d)", s->name, (int)s->pid);
        return;
    }
    printf("  no service named '%s'\n", name);
}

static void menu_resume(const char *name)
{
    for (int i = 0; i < service_count; i++) {
        struct service *s = &services[i];
        if (strcmp(s->name, name) != 0)
            continue;
        if (s->pid < 0 || !s->paused) {
            printf("  %s is not paused\n", name);
            return;
        }
        kill(s->pid, SIGCONT);              /* let it run again */
        s->paused = 0;
        logmsg("[%s] resumed with SIGCONT (pid %d)", s->name, (int)s->pid);
        return;
    }
    printf("  no service named '%s'\n", name);
}

static void handle_command(char *line)
{
    char *cmd, *arg, *save;

    cmd = strtok_r(line, " \t\n", &save);
    arg = strtok_r(NULL, " \t\n", &save);

    if (cmd == NULL)
        return;

    if (strcmp(cmd, "status") == 0 || strcmp(cmd, "s") == 0)
        show_status();
    else if (strcmp(cmd, "stop") == 0 && arg != NULL)
        menu_stop(arg);
    else if (strcmp(cmd, "start") == 0 && arg != NULL)
        menu_start(arg);
    else if (strcmp(cmd, "pause") == 0 && arg != NULL)
        menu_pause(arg);
    else if (strcmp(cmd, "resume") == 0 && arg != NULL)
        menu_resume(arg);
    else if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "q") == 0)
        raise(SIGTERM);
    else
        printf("  commands: status | start <name> | stop <name> | pause <name> | resume <name> | quit\n");
}

/* ---------------------------------------------------------------- signals */

static volatile sig_atomic_t quitting;

static void on_sigchld(int sig) { (void)sig; child_died = 1; }
static void on_sigterm(int sig) { (void)sig; quitting   = 1; }

static void setup_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags   = SA_RESTART;             /* do not break slow syscalls */
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = on_sigterm;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
}

/* ------------------------------------------------------------------- main */

static void shutdown_all(void)
{
    logmsg("shutting down");

    for (int i = 0; i < service_count; i++)
        if (services[i].pid > 0) {
            services[i].stopped_by_user = 1;
            kill(services[i].pid, SIGTERM);
        }

    /* Give the services a few seconds to exit, then stop waiting. */
    for (int i = 0; i < 50; i++) {
        int alive = 0;

        reap_children();
        for (int j = 0; j < service_count; j++)
            if (services[j].pid > 0)
                alive++;
        if (alive == 0)
            return;

        nanosleep(&(struct timespec){ 0, 100000000L }, NULL);
    }

    for (int i = 0; i < service_count; i++)
        if (services[i].pid > 0) {
            logmsg("[%s] not responding, sending SIGKILL", services[i].name);
            kill(services[i].pid, SIGKILL);
        }
    reap_children();
}

int main(int argc, char *argv[])
{
    const char   *config = (argc > 1) ? argv[1] : "services.conf";
    struct pollfd stdin_poll = { .fd = STDIN_FILENO, .events = POLLIN };

    if (load_config(config) <= 0) {
        fprintf(stderr, "warden: no services to supervise\n");
        return EXIT_FAILURE;
    }

    logfp = fopen("warden.log", "a");
    setup_signals();

    logmsg("warden started (pid %d), %d service(s) from '%s'",
           (int)getpid(), service_count, config);

    for (int i = 0; i < service_count; i++)
        start_service(&services[i]);

    printf("\n  commands: status | start <name> | stop <name> | pause <name> | resume <name> | quit\n\n");

    while (!quitting) {
        /* Wait up to one second for a typed command. The timeout also paces
         * the loop so restarts happen on schedule. */
        int ready = poll(&stdin_poll, 1, 1000);

        if (child_died) {
            child_died = 0;
            reap_children();
        }

        do_pending_restarts();

        if (ready > 0) {
            char line[MAX_LINE];
            if (fgets(line, sizeof line, stdin) == NULL)
                break;                      /* end of input */
            handle_command(line);
        }
    }

    shutdown_all();
    logmsg("warden stopped");

    if (logfp != NULL)
        fclose(logfp);
    return EXIT_SUCCESS;
}
