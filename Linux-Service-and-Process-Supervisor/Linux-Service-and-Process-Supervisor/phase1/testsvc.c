/*
 * testsvc.c - controllable dummy service used to test the supervisor.
 *
 * Usage: testsvc <mode> [seconds]
 *   ok     run, then exit with status 0   (normal termination)
 *   fail   run, then exit with status 3   (failure exit code)
 *   crash  run, then dereference NULL     (terminated by SIGSEGV)
 *   hang   run forever                    (must be killed manually)
 *
 * Build: gcc -Wall -Wextra -std=c11 -o testsvc testsvc.c
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    const char *mode = (argc > 1) ? argv[1] : "ok";
    int seconds      = (argc > 2) ? atoi(argv[2]) : 2;

    printf("    [testsvc pid=%d] mode=%s, running %ds\n",
           (int)getpid(), mode, seconds);
    fflush(stdout);

    if (strcmp(mode, "hang") == 0)
        for (;;)
            pause();

    sleep(seconds);

    if (strcmp(mode, "fail") == 0)
        return 3;

    if (strcmp(mode, "crash") == 0) {
        volatile int *p = NULL;
        *p = 42;                        /* raises SIGSEGV */
    }

    return 0;
}
