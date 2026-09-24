/*
 * wardenctl.c - administrative client for the Warden supervisor.
 *
 * Creates a private reply FIFO, sends one request line to the supervisor's
 * command FIFO, then blocks reading the reply. Two named pipes are used
 * because a FIFO is unidirectional.
 *
 * Usage: wardenctl [-p fifo] <status | start NAME | stop NAME |
 *                             restart NAME | shutdown>
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAXLINE 512

int main(int argc, char *argv[])
{
    char  cmdfifo[256] = "/tmp/warden.cmd";
    char  replyfifo[256];
    char  request[MAXLINE];
    char  buf[4096];
    int   fd;
    int   argi = 1;
    ssize_t r;

    if (argc > 2 && strcmp(argv[1], "-p") == 0) {
        strncpy(cmdfifo, argv[2], sizeof cmdfifo - 1);
        argi = 3;
    }

    if (argi >= argc) {
        fprintf(stderr,
                "usage: %s [-p fifo] <status|start NAME|stop NAME|"
                "restart NAME|shutdown>\n", argv[0]);
        return EXIT_FAILURE;
    }

    snprintf(replyfifo, sizeof replyfifo, "/tmp/warden-reply-%d",
             (int)getpid());

    if (mkfifo(replyfifo, 0600) < 0) {
        fprintf(stderr, "wardenctl: mkfifo failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    snprintf(request, sizeof request, "%s %s%s%s\n",
             replyfifo, argv[argi],
             (argi + 1 < argc) ? " " : "",
             (argi + 1 < argc) ? argv[argi + 1] : "");

    fd = open(cmdfifo, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "wardenctl: cannot open '%s': %s\n"
                        "(is the supervisor running?)\n",
                cmdfifo, strerror(errno));
        unlink(replyfifo);
        return EXIT_FAILURE;
    }

    if (write(fd, request, strlen(request)) < 0) {
        fprintf(stderr, "wardenctl: write failed: %s\n", strerror(errno));
        close(fd);
        unlink(replyfifo);
        return EXIT_FAILURE;
    }
    close(fd);

    /* Blocks until the supervisor opens the reply FIFO for writing. */
    fd = open(replyfifo, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "wardenctl: cannot read reply: %s\n",
                strerror(errno));
        unlink(replyfifo);
        return EXIT_FAILURE;
    }

    while ((r = read(fd, buf, sizeof buf)) > 0)
        if (write(STDOUT_FILENO, buf, (size_t)r) < 0)
            break;

    close(fd);
    unlink(replyfifo);
    return EXIT_SUCCESS;
}
