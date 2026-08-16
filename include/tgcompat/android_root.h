#ifndef TGCOMPAT_ANDROID_ROOT_H
#define TGCOMPAT_ANDROID_ROOT_H

#include <stdbool.h>

bool tgcompat_android_root_retry_flags(const char *path, int flags,
    int error, int *retry_flags);

#endif
