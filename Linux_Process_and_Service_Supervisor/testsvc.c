/*
 * testsvc.c - a dummy service used to test the supervisor.
 *
 * Usage: testsvc <mode> [seconds]
 *   ok     run for a few seconds, then exit normally
 *   fail   run for a few seconds, then exit with status 3
 *   crash  run for a few seconds, then crash with a segmentation fault
 *   long   keep running until asked to stop
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
    int seconds      = (argc > 2) ? atoi(argv[2]) : 3;

    if (strcmp(mode, "long") == 0) {
        signal(SIGTERM, on_term);
        while (!stop)
            sleep(1);
        return 0;
    }

    sleep(seconds);

    if (strcmp(mode, "fail") == 0)
        return 3;

    if (strcmp(mode, "crash") == 0) {
        volatile int *p = NULL;
        *p = 1;                      /* causes SIGSEGV */
    }

    return 0;
}
