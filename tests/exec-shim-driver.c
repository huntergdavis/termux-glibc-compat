#include <errno.h>
#include <stdio.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    char *child_arguments[] = {
        (char *)"tgcompat-preserved-argv0",
        (char *)"alpha",
        (char *)"beta",
        NULL,
    };

    if (argc != 2) {
        fprintf(stderr, "usage: %s TARGET\n", argv[0]);
        return 2;
    }
    execve(argv[1], child_arguments, environ);
    errno = errno == 0 ? EIO : errno;
    perror("exec-shim driver");
    return 111;
}
