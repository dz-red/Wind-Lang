#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        execl(argv[1], argv[1], (char *)0);
        perror("execl");
        _exit(127);
    }
    int status;
    struct rusage usage;
    while (wait4(pid, &status, 0, &usage) < 0) {
        if (errno == EINTR) continue;
        perror("wait4");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    printf("%ld\n", usage.ru_maxrss);
    return 0;
}
