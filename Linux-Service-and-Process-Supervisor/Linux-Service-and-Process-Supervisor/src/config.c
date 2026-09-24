/*
 * config.c - read the service configuration file.
 *
 * Line format:
 *     <name>  <policy>  <mem_mb>  <command>  [arguments...]
 *
 *     policy  : always | onfailure | never
 *     mem_mb  : address-space limit applied with setrlimit(), 0 = unlimited
 *
 * Blank lines and '#' comments are ignored. A malformed entry is reported
 * and skipped so that one bad line does not prevent the supervisor starting.
 */

#define _POSIX_C_SOURCE 200809L

#include "warden.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct service services[MAX_SERVICES];
int            service_count;

static int parse_policy(const char *s, enum policy *out)
{
    if (strcmp(s, "always") == 0)     { *out = P_ALWAYS;    return 0; }
    if (strcmp(s, "onfailure") == 0)  { *out = P_ONFAILURE; return 0; }
    if (strcmp(s, "never") == 0)      { *out = P_NEVER;     return 0; }
    return -1;
}

static int parse_line(const char *line, struct service *s)
{
    char *tok, *save;
    int   argc = 0;

    memset(s, 0, sizeof *s);
    strncpy(s->raw, line, MAX_LINE - 1);

    /* field 1: name */
    tok = strtok_r(s->raw, " \t\n", &save);
    if (tok == NULL)
        return -1;
    strncpy(s->name, tok, MAX_NAME - 1);

    /* field 2: restart policy */
    tok = strtok_r(NULL, " \t\n", &save);
    if (tok == NULL || parse_policy(tok, &s->policy) < 0)
        return -1;

    /* field 3: memory limit in MB */
    tok = strtok_r(NULL, " \t\n", &save);
    if (tok == NULL)
        return -1;
    s->mem_limit_mb = strtol(tok, NULL, 10);

    /* remaining fields: command and arguments */
    while ((tok = strtok_r(NULL, " \t\n", &save)) != NULL) {
        if (argc >= MAX_ARGS)
            return -1;
        s->argv[argc++] = tok;
    }
    if (argc == 0)
        return -1;
    s->argv[argc] = NULL;

    snprintf(s->logfile, sizeof s->logfile, "logs/%s.log", s->name);

    s->pid         = -1;
    s->state       = S_STOPPED;
    s->backoff     = BACKOFF_START;
    s->procstate   = '-';
    return 0;
}

int config_load(const char *path)
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
        char *hash;

        lineno++;

        hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';

        if (strspn(line, " \t\n") == strlen(line))
            continue;

        if (service_count >= MAX_SERVICES) {
            fprintf(stderr, "warden: service limit reached at line %d\n",
                    lineno);
            break;
        }

        if (parse_line(line, &services[service_count]) < 0) {
            fprintf(stderr, "warden: malformed entry at line %d, skipped\n",
                    lineno);
            continue;
        }

        service_count++;
    }

    fclose(fp);
    return service_count;
}
