#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *expected_proc_self_exe =
        getenv("TGCOMPAT_EXPECT_PROC_SELF_EXE");
    const char *proc_self_exe = getenv("TGCOMPAT_PROC_SELF_EXE");

    if (argc != 3 || strcmp(argv[0], "tgcompat-preserved-argv0") != 0 ||
            strcmp(argv[1], "alpha") != 0 || strcmp(argv[2], "beta") != 0) {
        fprintf(stderr, "exec shim did not preserve argv\n");
        return 1;
    }
    if (expected_proc_self_exe == NULL || proc_self_exe == NULL ||
            strcmp(proc_self_exe, expected_proc_self_exe) != 0) {
        fprintf(stderr, "exec shim did not preserve the executable path\n");
        return 1;
    }
    puts("exec shim target: PASS");
    return 0;
}
