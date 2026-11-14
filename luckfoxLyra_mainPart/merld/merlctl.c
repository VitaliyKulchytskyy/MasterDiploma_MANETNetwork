// merlctl: send one command to merld and print its JSON answer.
//   merlctl status
//   merlctl send 0xC71F Hello there
// Exit status: 0 if merld answered "ok": true, 1 otherwise.

#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define TIMEOUT_MS  5000

int main(int argc, char** argv) {
    const char* path = getenv("MERL_SOCKET");
    if (!path || !*path)
        path = "/run/merl/merld.sock";

    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-s") == 0) {
        path = argv[2];
        first = 3;
    }
    if (first >= argc) {
        fprintf(stderr,
                "usage: %s [-s socket] <command>\n"
                "commands: status | network get | network set <address> <mask> | peers get | peers clear |\n"
                "          position get | position set <longitude> <latitude> | gps status |\n"
                "          radio stats | links get |\n"
                "          chat list [after-id] [limit] | chat send <address|broadcast> <text> | chat clear |\n"
                "          raw <command> [hex payload]\n",
                argv[0]);
        return 2;
    }

    char line[512];
    size_t length = 0;
    for (int i = first; i < argc; ++i) {
        const int n = snprintf(line + length, sizeof(line) - length, "%s%s", i > first ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(line) - length - 1) {
            fprintf(stderr, "command too long\n");
            return 2;
        }
        length += (size_t)n;
    }
    line[length++] = '\n';

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (fd < 0 || connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        printf("{\"ok\":false,\"error\":\"merld is not running (%s: %s)\"}\n", path, strerror(errno));
        return 1;
    }
    if (send(fd, line, length, MSG_NOSIGNAL) != (ssize_t)length) {
        printf("{\"ok\":false,\"error\":\"cannot send to merld\"}\n");
        return 1;
    }

    // one JSON line back (a chat page can be large), then merld closes the connection
    size_t capacity = 16384, got = 0;
    char* answer = malloc(capacity);
    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        if (!answer || poll(&pfd, 1, TIMEOUT_MS) <= 0) {
            printf("{\"ok\":false,\"error\":\"merld did not answer\"}\n");
            return 1;
        }
        if (capacity - got < 4096) {
            char* bigger = realloc(answer, capacity *= 2);
            if (!bigger)
                break;
            answer = bigger;
        }
        const ssize_t n = recv(fd, answer + got, capacity - 1 - got, 0);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    answer[got] = '\0';

    fputs(answer, stdout);
    return strncmp(answer, "{\"ok\":true", 10) == 0 ? 0 : 1;
}
