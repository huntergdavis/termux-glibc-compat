#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>

#include "tgcompat/android_root.h"

int main(void) {
    int retry_flags = -1;
    int original = O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY;

    assert(unsetenv("TGCOMPAT_ANDROID_ROOT_O_PATH") == 0);
    assert(!tgcompat_android_root_retry_flags("/proc/self/root", original,
        EACCES, &retry_flags));
    assert(setenv("TGCOMPAT_ANDROID_ROOT_O_PATH", "0", 1) == 0);
    assert(!tgcompat_android_root_retry_flags("/proc/self/root", original,
        EACCES, &retry_flags));
    assert(setenv("TGCOMPAT_ANDROID_ROOT_O_PATH", "1", 1) == 0);
    assert(tgcompat_android_root_retry_flags("/proc/self/root", original,
        EACCES, &retry_flags));
    assert(retry_flags == (O_PATH | O_DIRECTORY | O_CLOEXEC));
    assert(tgcompat_android_root_retry_flags("/proc/self/root/", original,
        EPERM, &retry_flags));
    assert(!tgcompat_android_root_retry_flags("/proc/self/status", original,
        EACCES, &retry_flags));
    assert(!tgcompat_android_root_retry_flags("/proc/self/root",
        O_WRONLY | O_DIRECTORY, EACCES, &retry_flags));
    assert(!tgcompat_android_root_retry_flags("/proc/self/root", O_RDONLY,
        EACCES, &retry_flags));
    assert(!tgcompat_android_root_retry_flags("/proc/self/root", original,
        ENOENT, &retry_flags));
    assert(!tgcompat_android_root_retry_flags("/proc/self/root",
        original | O_PATH, EACCES, &retry_flags));

    puts("Android real-root shim policy: PASS");
    return 0;
}
