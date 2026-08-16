#define _GNU_SOURCE

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    char *child_arguments[] = {
        (char *)"tgcompat-preserved-argv0",
        (char *)"alpha",
        (char *)"beta",
        NULL,
    };

    pid_t child;
    int status;
    int result;

    if (argc != 3) {
        fprintf(stderr, "usage: %s MODE TARGET\n", argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "execve") == 0) {
        execve(argv[2], child_arguments, environ);
    } else if (strcmp(argv[1], "execv") == 0) {
        execv(argv[2], child_arguments);
    } else if (strcmp(argv[1], "execvp") == 0) {
        execvp(argv[2], child_arguments);
    } else if (strcmp(argv[1], "execvpe") == 0) {
        execvpe(argv[2], child_arguments, environ);
    } else if (strcmp(argv[1], "execl") == 0) {
        execl(argv[2], child_arguments[0], child_arguments[1],
            child_arguments[2], (char *)NULL);
    } else if (strcmp(argv[1], "posix_spawn") == 0) {
        result = posix_spawn(&child, argv[2], NULL, NULL, child_arguments,
            environ);
        if (result != 0) {
            errno = result;
            perror("exec-shim posix_spawn");
            return 111;
        }
        if (waitpid(child, &status, 0) != child) {
            perror("exec-shim waitpid");
            return 111;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 111;
    } else if (strcmp(argv[1], "posix_spawnp") == 0) {
        result = posix_spawnp(&child, argv[2], NULL, NULL, child_arguments,
            environ);
        if (result != 0) {
            errno = result;
            perror("exec-shim posix_spawnp");
            return 111;
        }
        if (waitpid(child, &status, 0) != child) {
            perror("exec-shim waitpid");
            return 111;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 111;
    } else {
        fprintf(stderr, "unknown exec-shim mode: %s\n", argv[1]);
        return 2;
    }
    errno = errno == 0 ? EIO : errno;
    perror("exec-shim driver");
    return 111;
}
