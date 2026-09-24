/*
 * log.c - timestamped, mutex-protected activity log.
 *
 * Every thread writes through log_msg(), so the log file is a serialised
 * record of supervisor activity. The mutex here is separate from the service
 * table lock; lock ordering is documented in docs/DESIGN.md.
 */

#define _POSIX_C_SOURCE 200809L

#include "warden.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static FILE            *logfp;
static int              echo_stdout = 1;
static pthread_mutex_t  log_lock = PTHREAD_MUTEX_INITIALIZER;

int log_open(const char *path, int echo)
{
    echo_stdout = echo;

    if (path != NULL) {
        logfp = fopen(path, "a");
        if (logfp == NULL)
            return -1;
        setvbuf(logfp, NULL, _IOLBF, 0);   /* line buffered */
    }
    return 0;
}

void log_msg(const char *fmt, ...)
{
    char      stamp[32];
    time_t    now = time(NULL);
    struct tm tm;
    va_list   ap;

    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%H:%M:%S", &tm);

    pthread_mutex_lock(&log_lock);

    if (logfp != NULL) {
        fprintf(logfp, "%s ", stamp);
        va_start(ap, fmt);
        vfprintf(logfp, fmt, ap);
        va_end(ap);
        fputc('\n', logfp);
    }

    if (echo_stdout) {
        printf("%s ", stamp);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        putchar('\n');
        fflush(stdout);
    }

    pthread_mutex_unlock(&log_lock);
}

void log_close(void)
{
    if (logfp != NULL) {
        fclose(logfp);
        logfp = NULL;
    }
}
