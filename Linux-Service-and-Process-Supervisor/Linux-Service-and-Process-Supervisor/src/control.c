/*
 * control.c - administrative control channel over named pipes (FIFOs).
 *
 * The supervisor runs detached from any terminal, so control must arrive
 * through a persistent IPC object in the filesystem. A FIFO is the natural
 * fit: it has a pathname, survives client restarts, and needs no shared
 * ancestry between the two processes.
 *
 * Request  (client -> supervisor):  "<reply_fifo> <command> [argument]\n"
 * Response (supervisor -> client):  free-form text on the reply FIFO
 *
 * The request FIFO is opened O_RDWR so that the supervisor itself always
 * holds a writer. Without this, read() would return end-of-file every time
 * the last client disconnected, turning the poll loop into a busy spin.
 */

#define _POSIX_C_SOURCE 200809L

#include "warden.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char control_fifo[MAX_PATH] = "/tmp/warden.cmd";

/* ------------------------------------------------------------ responses */

/*
 * Open the client's reply FIFO and send the response. O_NONBLOCK makes
 * open() fail with ENXIO rather than block when no reader is present, so a
 * client that died cannot stall the control thread. A short retry covers the
 * normal race where the client has not yet reached its own open().
 */
static void send_reply(const char *path, const char *text)
{
    int fd = -1;

    for (int attempt = 0; attempt < 20; attempt++) {
        fd = open(path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0)
            break;
        if (errno != ENXIO)
            return;
        msleep(50);
    }

    if (fd < 0) {
        log_msg("control: client reply pipe '%s' never opened", path);
        return;
    }

    if (write(fd, text, strlen(text)) < 0)
        log_msg("control: reply write failed: %s", strerror(errno));

    close(fd);
}

/* ------------------------------------------------------------- commands */

static void cmd_status(char *out, size_t cap)
{
    size_t used;

    used = snprintf(out, cap,
                    "%-12s %-8s %-8s %-7s %-8s %-8s %s\n",
                    "SERVICE", "STATE", "PID", "CPU%", "RSS(kB)",
                    "RESTARTS", "FAULTS(min/maj)");

    pthread_mutex_lock(&table_lock);

    for (int i = 0; i < service_count && used < cap; i++) {
        struct service *s = &services[i];
        char            pidbuf[16];

        if (s->pid > 0)
            snprintf(pidbuf, sizeof pidbuf, "%d", (int)s->pid);
        else
            snprintf(pidbuf, sizeof pidbuf, "-");

        used += snprintf(out + used, cap - used,
                         "%-12s %-8s %-8s %-7.1f %-8ld %-8d %lu/%lu\n",
                         s->name, state_name(s->state), pidbuf,
                         s->cpu_pct, s->rss_kb, s->restarts,
                         s->minflt, s->majflt);
    }

    pthread_mutex_unlock(&table_lock);
}

static void cmd_start(const char *name, char *out, size_t cap)
{
    struct service *s;
    int             ok = 0;

    pthread_mutex_lock(&table_lock);
    s = service_find(name);
    if (s != NULL && s->pid <= 0) {
        s->burst   = 0;
        s->backoff = BACKOFF_START;
        s->state   = S_STOPPED;
        ok = 1;
    }
    pthread_mutex_unlock(&table_lock);

    if (s == NULL)
        snprintf(out, cap, "no such service: %s\n", name);
    else if (!ok)
        snprintf(out, cap, "%s is already running (pid %d)\n",
                 name, (int)s->pid);
    else {
        service_request_start(s);        /* takes semaphore, then lock */
        snprintf(out, cap, "started %s\n", name);
    }
}

static void cmd_stop(const char *name, char *out, size_t cap)
{
    struct service *s;

    pthread_mutex_lock(&table_lock);
    s = service_find(name);
    if (s == NULL) {
        pthread_mutex_unlock(&table_lock);
        snprintf(out, cap, "no such service: %s\n", name);
        return;
    }
    if (s->pid <= 0) {
        s->state = S_STOPPED;
        s->manual_stop = 1;
        pthread_mutex_unlock(&table_lock);
        snprintf(out, cap, "%s is not running\n", name);
        return;
    }
    s->manual_stop = 1;                  /* suppresses automatic restart */
    service_stop(s, SIGTERM);
    pthread_mutex_unlock(&table_lock);

    snprintf(out, cap, "stopping %s\n", name);
}

static void cmd_restart(const char *name, char *out, size_t cap)
{
    struct service *s;

    pthread_mutex_lock(&table_lock);
    s = service_find(name);
    if (s == NULL) {
        pthread_mutex_unlock(&table_lock);
        snprintf(out, cap, "no such service: %s\n", name);
        return;
    }
    if (s->pid > 0) {
        s->manual_stop = 1;
        service_stop(s, SIGTERM);
    }
    pthread_mutex_unlock(&table_lock);

    /* Wait for the reaper to collect it before starting again. */
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&table_lock);
        pid_t p = s->pid;
        pthread_mutex_unlock(&table_lock);
        if (p <= 0)
            break;
        msleep(100);
    }

    pthread_mutex_lock(&table_lock);
    s->manual_stop = 0;
    s->burst       = 0;
    s->backoff     = BACKOFF_START;
    pthread_mutex_unlock(&table_lock);

    service_request_start(s);
    snprintf(out, cap, "restarted %s\n", name);
}

/* --------------------------------------------------------------- thread */

static void dispatch(char *line)
{
    char *reply_path, *cmd, *arg, *save;
    char  out[4096];

    out[0] = '\0';

    reply_path = strtok_r(line, " \t\n", &save);
    cmd        = strtok_r(NULL, " \t\n", &save);
    arg        = strtok_r(NULL, " \t\n", &save);

    if (reply_path == NULL || cmd == NULL)
        return;

    log_msg("control: command '%s%s%s'",
            cmd, arg ? " " : "", arg ? arg : "");

    if (strcmp(cmd, "status") == 0) {
        cmd_status(out, sizeof out);
    } else if (strcmp(cmd, "shutdown") == 0) {
        snprintf(out, sizeof out, "shutting down\n");
        send_reply(reply_path, out);
        kill(getpid(), SIGTERM);         /* routed through the self-pipe */
        return;
    } else if (arg == NULL) {
        snprintf(out, sizeof out, "usage: %s <service>\n", cmd);
    } else if (strcmp(cmd, "start") == 0) {
        cmd_start(arg, out, sizeof out);
    } else if (strcmp(cmd, "stop") == 0) {
        cmd_stop(arg, out, sizeof out);
    } else if (strcmp(cmd, "restart") == 0) {
        cmd_restart(arg, out, sizeof out);
    } else {
        snprintf(out, sizeof out, "unknown command: %s\n", cmd);
    }

    send_reply(reply_path, out);
}

void *control_thread(void *arg)
{
    int           fd;
    struct pollfd pfd;
    char          buf[MAX_LINE];

    (void)arg;

    unlink(control_fifo);
    if (mkfifo(control_fifo, 0666) < 0) {
        log_msg("control: mkfifo '%s' failed: %s",
                control_fifo, strerror(errno));
        return NULL;
    }

    fd = open(control_fifo, O_RDWR);     /* see comment at top of file */
    if (fd < 0) {
        log_msg("control: open failed: %s", strerror(errno));
        return NULL;
    }

    log_msg("control channel ready at %s", control_fifo);

    pfd.fd     = fd;
    pfd.events = POLLIN;

    while (!shutting_down) {
        int n = poll(&pfd, 1, 500);

        if (n <= 0)
            continue;

        ssize_t r = read(fd, buf, sizeof buf - 1);
        if (r <= 0)
            continue;
        buf[r] = '\0';

        /* A client may send several lines; handle each one. */
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save);
             line != NULL;
             line = strtok_r(NULL, "\n", &save))
            dispatch(line);
    }

    close(fd);
    unlink(control_fifo);
    return NULL;
}
