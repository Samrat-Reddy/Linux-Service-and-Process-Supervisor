/*
 * testsvc.c - controllable dummy service for testing the supervisor.
 *
 * Usage: testsvc <mode> [seconds]
 *   ok      run, exit 0                     (normal termination)
 *   fail    run, exit 3                     (failure exit code)
 *   crash   run, dereference NULL           (terminated by SIGSEGV)
 *   hang    run forever, ignore SIGTERM     (forces SIGKILL escalation)
 *   long    run forever, honour SIGTERM     (well-behaved daemon)
 *   flap    exit 1 immediately              (triggers restart storm)
 *   hog     spin on the CPU                 (high CPU sample)
 *   leak    allocate memory continuously    (trips the RSS limit)
 */

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_term(int sig) { (void)sig; stop = 1; }

int main(int argc, char *argv[])
{
    const char *mode = (argc > 1) ? argv[1] : "ok";
    int seconds      = (argc > 2) ? atoi(argv[2]) : 2;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("testsvc pid=%d mode=%s\n", (int)getpid(), mode);

    if (strcmp(mode, "flap") == 0)
        return 1;

    if (strcmp(mode, "hang") == 0) {
        signal(SIGTERM, SIG_IGN);           /* refuses to stop politely */
        for (;;)
            pause();
    }

    if (strcmp(mode, "long") == 0) {
        signal(SIGTERM, on_term);
        while (!stop)
            sleep(1);
        printf("testsvc pid=%d shutting down cleanly\n", (int)getpid());
        return 0;
    }

    if (strcmp(mode, "hog") == 0) {
        volatile double x = 0;
        signal(SIGTERM, on_term);
        while (!stop)
            x += 1.0;
        return 0;
    }

    if (strcmp(mode, "leak") == 0) {
        size_t total = 0;
        signal(SIGTERM, on_term);
        while (!stop) {
            char *p = malloc(4 * 1024 * 1024);
            if (p == NULL) {
                printf("malloc refused after %zuMB (rlimit reached)\n", total);
                sleep(2);
                continue;
            }
            memset(p, 1, 4 * 1024 * 1024);  /* touch it so RSS grows */
            total += 4;
            printf("allocated %zuMB\n", total);
            sleep(1);
        }
        return 0;
    }

    sleep(seconds);

    if (strcmp(mode, "fail") == 0)
        return 3;

    if (strcmp(mode, "crash") == 0) {
        volatile int *p = NULL;
        *p = 42;
    }

    return 0;
}
