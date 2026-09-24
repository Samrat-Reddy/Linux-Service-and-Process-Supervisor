/*
 * cowdemo.c - measures demand paging and copy-on-write using page faults.
 *
 * Both effects are invisible in ordinary program output but are counted by
 * the kernel and reported through getrusage(). A minor fault is a fault
 * resolved without disk I/O: either a first touch of an anonymous page
 * (demand paging) or a write to a page shared read-only after fork()
 * (copy-on-write).
 *
 * Build: gcc -Wall -Wextra -std=c11 -o cowdemo cowdemo.c
 * Run:   ./cowdemo [megabytes]
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static long minor_faults(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_minflt;
}

int main(int argc, char *argv[])
{
    size_t mb        = (argc > 1) ? (size_t)atoi(argv[1]) : 64;
    size_t bytes     = mb * 1024 * 1024;
    long   pagesize  = sysconf(_SC_PAGESIZE);
    size_t pages     = bytes / (size_t)pagesize;
    char  *buf;
    long   before, after;

    /* Line-buffer stdout. The child ends with _exit(), which does not flush
     * stdio buffers, so buffered output would otherwise be lost when this
     * program's output is a pipe or file rather than a terminal. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("page size   : %ld bytes\n", pagesize);
    printf("buffer      : %zu MB (%zu pages)\n\n", mb, pages);

    /* ---- demand paging ------------------------------------------------ */

    before = minor_faults();
    buf = malloc(bytes);
    if (buf == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }
    after = minor_faults();
    printf("after malloc (untouched)   : %ld minor faults\n", after - before);
    printf("  the kernel has reserved address space but mapped no pages\n\n");

    before = minor_faults();
    memset(buf, 1, bytes);
    after = minor_faults();
    printf("after touching every page  : %ld minor faults\n", after - before);
    printf("  each first touch raised a fault; pages are supplied on demand\n\n");

    /* ---- copy-on-write ------------------------------------------------ */

    printf("forking...\n\n");
    fflush(stdout);

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return EXIT_FAILURE;
    }

    if (pid == 0) {
        long b, a;

        b = minor_faults();
        for (size_t i = 0; i < bytes; i += (size_t)pagesize)
            if (buf[i] != 1)                 /* read only */
                abort();
        a = minor_faults();
        printf("child reading all pages    : %ld minor faults\n", a - b);
        printf("  reads are served from the pages shared with the parent\n\n");

        b = minor_faults();
        memset(buf, 2, bytes);               /* now write */
        a = minor_faults();
        printf("child writing all pages    : %ld minor faults\n", a - b);
        printf("  each write forced a private copy: copy-on-write\n");

        _exit(EXIT_SUCCESS);
    }

    wait(NULL);
    free(buf);
    return EXIT_SUCCESS;
}
