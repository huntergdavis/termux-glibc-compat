#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3 || strcmp(argv[0], "tgcompat-preserved-argv0") != 0 ||
            strcmp(argv[1], "alpha") != 0 || strcmp(argv[2], "beta") != 0) {
        fprintf(stderr, "exec shim did not preserve argv\n");
        return 1;
    }
    puts("exec shim target: PASS");
    return 0;
}
