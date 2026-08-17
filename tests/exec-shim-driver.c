#define _GNU_SOURCE

#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static char **environment_without(const char *name) {
    size_t count = 0;
    size_t index;
    size_t output = 0;
    size_t name_length = strlen(name);
    char **filtered;

    while (environ[count] != NULL) {
        ++count;
    }
    filtered = calloc(count + 1U, sizeof(*filtered));
    if (filtered == NULL) {
        return NULL;
    }
    for (index = 0; index < count; ++index) {
        if (strncmp(environ[index], name, name_length) == 0 &&
                environ[index][name_length] == '=') {
            continue;
        }
        filtered[output++] = environ[index];
    }
    filtered[output] = NULL;
    return filtered;
}

static char **environment_without_two(const char *first, const char *second) {
    size_t count = 0;
    size_t index;
    size_t output = 0;
    size_t first_length = strlen(first);
    size_t second_length = strlen(second);
    char **filtered;

    while (environ[count] != NULL) {
        ++count;
    }
    filtered = calloc(count + 1U, sizeof(*filtered));
    if (filtered == NULL) {
        return NULL;
    }
    for (index = 0; index < count; ++index) {
        if ((strncmp(environ[index], first, first_length) == 0 &&
                environ[index][first_length] == '=') ||
                (strncmp(environ[index], second, second_length) == 0 &&
                    environ[index][second_length] == '=')) {
            continue;
        }
        filtered[output++] = environ[index];
    }
    filtered[output] = NULL;
    return filtered;
}

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
    } else if (strcmp(argv[1], "execve-filter-path-control") == 0) {
        char **filtered = environment_without_two(
            "TGCOMPAT_EXEC_PATH_FROM", "TGCOMPAT_EXEC_PATH_TO");

        if (filtered == NULL) {
            perror("exec-shim filtered path environment");
            return 111;
        }
        execve(argv[2], child_arguments, filtered);
        free(filtered);
    } else if (strcmp(argv[1], "execve-filter-control") == 0) {
        char **filtered = environment_without("TGCOMPAT_EXEC_LD_PRELOAD");

        if (filtered == NULL) {
            perror("exec-shim filtered environment");
            return 111;
        }
        execve(argv[2], child_arguments, filtered);
        free(filtered);
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
