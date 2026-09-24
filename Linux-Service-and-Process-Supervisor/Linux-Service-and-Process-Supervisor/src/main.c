/*
 * main.c - supervisor entry point.
 *
 * Starts four threads over one shared service table:
 *
 *   main     restart manager - waits on a condition variable for services
 *            whose backoff delay has expired
 *   reaper   reads the self-pipe, collects terminated children, applies
 *            the restart policy
 *   health   samples /proc every few seconds
 *   control  serves administrator commands from the FIFO
 */

#define _POSIX_C_SOURCE 200809L

#include "warden.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int foreground = 1;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-c config] [-l logfile] [-p fifo] [-d]\n"
            "  -c  service configuration file (default warden.conf)\n"
            "  -l  supervisor log file        (default logs/warden.log)\n"
            "  -p  control FIFO path          (default /tmp/warden.cmd)\n"
            "  -d  run as a background daemon\n", prog);
}

/*
 * Detach from the controlling terminal.
 *
 * The first fork() guarantees the process is not a process-group leader, so
 * setsid() can succeed and create a new session with no controlling
 * terminal. The second fork() ensures the daemon is not a session leader
 * either, so it can never acquire one later.
 *
 * The working directory is deliberately kept, because service commands in
 * the configuration file may be relative paths.
 */
static int daemonise(void)
{
    pid_t pid;
    int   fd;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(EXIT_SUCCESS);

    if (setsid() < 0)
        return -1;

    pid = fork();
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(EXIT_SUCCESS);

    umask(0027);

    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    const char *config  = "warden.conf";
    const char *logfile = "logs/warden.log";
    int         daemon_mode = 0;
    int         opt;
    pthread_t   t_reaper, t_health, t_control;

    while ((opt = getopt(argc, argv, "c:l:p:dh")) != -1) {
        switch (opt) {
        case 'c': config  = optarg; break;
        case 'l': logfile = optarg; break;
        case 'p': strncpy(control_fifo, optarg, MAX_PATH - 1); break;
        case 'd': daemon_mode = 1;  break;
        case 'h': usage(argv[0]);   return EXIT_SUCCESS;
        default:  usage(argv[0]);   return EXIT_FAILURE;
        }
    }

    if (mkdir("logs", 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "warden: cannot create logs/: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (config_load(config) <= 0) {
        fprintf(stderr, "warden: no services configured\n");
        return EXIT_FAILURE;
    }

    if (daemon_mode) {
        if (daemonise() < 0) {
            fprintf(stderr, "warden: daemonise failed: %s\n", strerror(errno));
            return EXIT_FAILURE;
        }
        foreground = 0;
    }

    if (log_open(logfile, foreground) < 0) {
        fprintf(stderr, "warden: cannot open log '%s': %s\n",
                logfile, strerror(errno));
        return EXIT_FAILURE;
    }

    if (signals_init() < 0)
        return EXIT_FAILURE;

    if (sem_init(&restart_slots, 0, RESTART_SLOTS) < 0) {
        perror("warden: sem_init");
        return EXIT_FAILURE;
    }

    log_msg("warden starting, pid=%d, %d service(s) from '%s'",
            (int)getpid(), service_count, config);

    if (pthread_create(&t_reaper, NULL, reaper_thread, NULL) != 0 ||
        pthread_create(&t_health, NULL, health_thread, NULL) != 0 ||
        pthread_create(&t_control, NULL, control_thread, NULL) != 0) {
        fprintf(stderr, "warden: pthread_create failed\n");
        return EXIT_FAILURE;
    }

    /* Initial launch of every configured service. */
    for (int i = 0; i < service_count; i++)
        service_request_start(&services[i]);

    supervisor_run();               /* blocks until shutdown is requested */

    supervisor_shutdown();

    pthread_join(t_reaper,  NULL);
    pthread_join(t_health,  NULL);
    pthread_join(t_control, NULL);

    sem_destroy(&restart_slots);
    log_msg("warden stopped");
    log_close();

    return EXIT_SUCCESS;
}
