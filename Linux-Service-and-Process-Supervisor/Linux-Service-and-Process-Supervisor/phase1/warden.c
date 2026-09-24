/*
 * warden.c - Linux Service and Process Supervisor (Phase 1)
 *
 * Phase 1 scope: read a service configuration file, launch each service as a
 * child process, wait for every child to terminate, and report how it died.
 * Single-threaded and blocking. No automatic restart (Phase 2).
 *
 * Concepts: CO-1 (fork/exec/wait as the shell does), CO-2 (process lifecycle,
 * exit-status decoding, zombie prevention), CO-5 (file I/O on the config file).
 *
 * Build: gcc -Wall -Wextra -std=c11 -o warden warden.c
 * Run:   ./warden services.conf
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#define MAX_SERVICES 16   /* services the supervisor can manage   */
#define MAX_ARGS     16   /* argv entries per service             */
#define MAX_LINE     256  /* longest configuration line           */
#define MAX_NAME     32   /* longest service name                 */

/* One managed service. argv[] points into raw[], which holds the tokenised
 * configuration line, so the strings stay valid for the process lifetime. */
struct service {
    char  name[MAX_NAME];      /* logical name from the config file */
    char  raw[MAX_LINE];       /* tokenised copy of the config line */
    char *argv[MAX_ARGS + 1];  /* argument vector for execvp()      */
    pid_t pid;                 /* child PID, -1 if not started      */
    int   running;             /* 1 while the child is alive        */
};

static struct service services[MAX_SERVICES];
static int service_count = 0;

/* ---------------------------------------------------------------- config */

/*
 * Split one configuration line into a service name and an argument vector.
 * Format:  <name>  <command>  [args...]
 * Returns 0 on success, -1 if the line is unusable.
 */
static int parse_line(const char *line, struct service *s)
{
    char *tok, *save;
    int argc = 0;

    strncpy(s->raw, line, MAX_LINE - 1);
    s->raw[MAX_LINE - 1] = '\0';

    tok = strtok_r(s->raw, " \t\n", &save);
    if (tok == NULL)
        return -1;                      /* nothing on the line */

    strncpy(s->name, tok, MAX_NAME - 1);
    s->name[MAX_NAME - 1] = '\0';

    while ((tok = strtok_r(NULL, " \t\n", &save)) != NULL) {
        if (argc >= MAX_ARGS)
            return -1;                  /* too many arguments */
        s->argv[argc++] = tok;
    }

    if (argc == 0)
        return -1;                      /* name given but no command */

    s->argv[argc] = NULL;               /* execvp() needs a NULL terminator */
    s->pid = -1;
    s->running = 0;
    return 0;
}

/*
 * Read the configuration file into the service table.
 * Blank lines and '#' comments are ignored; a malformed entry is reported
 * and skipped rather than aborting the whole supervisor.
 * Returns the number of services loaded, or -1 if the file cannot be opened.
 */
static int load_config(const char *path)
{
    FILE *fp;
    char line[MAX_LINE];
    int lineno = 0;

    fp = fopen(path, "r");
    if (fp == NULL) {
        fprintf(stderr, "warden: cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof line, fp) != NULL) {
        char *hash;

        lineno++;

        hash = strchr(line, '#');       /* strip trailing comment */
        if (hash != NULL)
            *hash = '\0';

        if (strspn(line, " \t\n") == strlen(line))
            continue;                   /* blank line */

        if (service_count >= MAX_SERVICES) {
            fprintf(stderr, "warden: service limit (%d) reached, "
                    "ignoring line %d\n", MAX_SERVICES, lineno);
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

/* --------------------------------------------------------------- process */

/*
 * Create a child process and replace its image with the service program.
 * fork() gives a duplicate of the supervisor; execvp() then overwrites the
 * child's program while keeping its PID. Returns 0 on success, -1 on failure.
 */
static int start_service(struct service *s)
{
    pid_t pid;

    pid = fork();

    if (pid < 0) {
        fprintf(stderr, "warden: fork failed for '%s': %s\n",
                s->name, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child: execvp() only returns if the exec failed. */
        execvp(s->argv[0], s->argv);
        fprintf(stderr, "warden: exec '%s' failed: %s\n",
                s->argv[0], strerror(errno));
        _exit(127);                     /* 127 = command not executable */
    }

    /* Parent */
    s->pid = pid;
    s->running = 1;
    printf("[warden] started  %-10s pid=%-6d cmd=%s\n",
           s->name, (int)pid, s->argv[0]);
    return 0;
}

/* Locate a service by the PID returned from waitpid(). */
static struct service *find_by_pid(pid_t pid)
{
    for (int i = 0; i < service_count; i++)
        if (services[i].running && services[i].pid == pid)
            return &services[i];
    return NULL;
}

/*
 * Decode the status word returned by waitpid().
 * The kernel encodes both the exit code and the terminating signal in this
 * word, so it must be interpreted with the W* macros, never read directly.
 */
static void report_termination(const struct service *s, int status)
{
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        printf("[warden] exited   %-10s pid=%-6d status=%d (%s)\n",
               s->name, (int)s->pid, code,
               code == 0 ? "normal" : "failure");
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        printf("[warden] killed   %-10s pid=%-6d signal=%d (%s)%s\n",
               s->name, (int)s->pid, sig, strsignal(sig),
#ifdef WCOREDUMP
               WCOREDUMP(status) ? " [core dumped]" : "");
#else
               "");
#endif
    } else {
        printf("[warden] unknown  %-10s pid=%-6d raw status=0x%x\n",
               s->name, (int)s->pid, status);
    }
}

/* ------------------------------------------------------------------ main */

int main(int argc, char *argv[])
{
    const char *config = (argc > 1) ? argv[1] : "services.conf";
    int live = 0;

    if (load_config(config) <= 0) {
        fprintf(stderr, "warden: no services to supervise\n");
        return EXIT_FAILURE;
    }

    printf("[warden] supervisor pid=%d, %d service(s) from '%s'\n\n",
           (int)getpid(), service_count, config);

    for (int i = 0; i < service_count; i++)
        if (start_service(&services[i]) == 0)
            live++;

    if (live == 0) {
        fprintf(stderr, "warden: no service could be started\n");
        return EXIT_FAILURE;
    }

    putchar('\n');

    /*
     * Reap every child. Calling waitpid() for each one collects its exit
     * status and releases the kernel's process-table entry, which is what
     * prevents terminated children from remaining as zombies.
     */
    while (live > 0) {
        int status;
        pid_t pid;
        struct service *s;

        pid = waitpid(-1, &status, 0);

        if (pid < 0) {
            if (errno == EINTR)
                continue;               /* interrupted by a signal, retry */
            if (errno == ECHILD)
                break;                  /* no children left */
            fprintf(stderr, "warden: waitpid failed: %s\n", strerror(errno));
            break;
        }

        s = find_by_pid(pid);
        if (s == NULL) {
            printf("[warden] reaped unknown child pid=%d\n", (int)pid);
            continue;
        }

        s->running = 0;
        report_termination(s, status);
        live--;
    }

    printf("\n[warden] all services terminated, supervisor exiting\n");
    return EXIT_SUCCESS;
}
