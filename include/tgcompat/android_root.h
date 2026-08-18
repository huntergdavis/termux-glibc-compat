#ifndef TGCOMPAT_ANDROID_ROOT_H
#define TGCOMPAT_ANDROID_ROOT_H

#include <stdbool.h>
#include <stddef.h>

bool tgcompat_android_root_retry_flags(const char *path, int flags,
    int error, int *retry_flags);
const char *tgcompat_android_root_rewrite_proc_net(const char *path,
    char *output, size_t output_size);

#endif
