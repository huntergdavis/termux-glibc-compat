#ifndef TGCOMPAT_TEST_TEMP_PATH_H
#define TGCOMPAT_TEST_TEMP_PATH_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    TGC_TEST_PATH_CAPACITY = 4096,
};

static int tgc_test_temp_template(char *output, size_t output_size,
                                  const char *name)
{
    const char *parent = getenv("TMPDIR");
    if (parent == NULL || parent[0] == '\0') {
        parent = "/tmp";
    }
    if (parent[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    int written = snprintf(output, output_size, "%s/%s.XXXXXX", parent, name);
    if (written < 0 || (size_t)written >= output_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

#endif
