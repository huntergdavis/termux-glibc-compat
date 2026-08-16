#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "tgcompat/android_root.h"

typedef int (*openat_function)(int, const char *, int, ...);
typedef int (*openat_checked_function)(int, const char *, int);

static openat_function real_openat;
static openat_function real_openat64;
static openat_checked_function real_openat_2;
static openat_checked_function real_openat64_2;

static void *next_symbol(const char *name) {
    return dlsym(RTLD_NEXT, name);
}

static void resolve_function(void *symbol, void *destination,
        size_t destination_size) {
    _Static_assert(sizeof(openat_function) == sizeof(void *),
        "function and data pointers must have equal size");
    memcpy(destination, &symbol, destination_size);
}

__attribute__((constructor)) static void initialize_android_root_shim(void) {
    void *symbol;

    symbol = next_symbol("openat");
    resolve_function(symbol, &real_openat, sizeof(real_openat));
    symbol = next_symbol("openat64");
    resolve_function(symbol, &real_openat64, sizeof(real_openat64));
    symbol = next_symbol("__openat_2");
    resolve_function(symbol, &real_openat_2, sizeof(real_openat_2));
    symbol = next_symbol("__openat64_2");
    resolve_function(symbol, &real_openat64_2, sizeof(real_openat64_2));
}

static bool enabled(void) {
    const char *value = getenv("TGCOMPAT_ANDROID_ROOT_O_PATH");

    return value != NULL && strcmp(value, "1") == 0;
}

bool tgcompat_android_root_retry_flags(const char *path, int flags,
        int error, int *retry_flags) {
    int allowed_flags = O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;

    if (!enabled() || path == NULL || retry_flags == NULL ||
            (error != EACCES && error != EPERM) ||
            (strcmp(path, "/proc/self/root") != 0 &&
                strcmp(path, "/proc/self/root/") != 0) ||
            (flags & O_DIRECTORY) == 0 || (flags & O_PATH) != 0 ||
            (flags & O_ACCMODE) != O_RDONLY) {
        return false;
    }
    *retry_flags = O_PATH | (flags & allowed_flags);
    return true;
}

static int retry_openat(openat_function function, int directory,
        const char *path, int flags, int result) {
    int retry_flags;
    int saved_errno = errno;

    if (result < 0 && function != NULL &&
            tgcompat_android_root_retry_flags(path, flags, saved_errno,
                &retry_flags)) {
        return function(directory, path, retry_flags);
    }
    errno = saved_errno;
    return result;
}

static bool flags_have_mode(int flags) {
    if ((flags & O_CREAT) != 0) {
        return true;
    }
#ifdef O_TMPFILE
    if ((flags & O_TMPFILE) == O_TMPFILE) {
        return true;
    }
#endif
    return false;
}

static int call_openat(openat_function function, int directory,
        const char *path, int flags, va_list arguments) {
    int result;

    if (function == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (flags_have_mode(flags)) {
        mode_t mode = va_arg(arguments, mode_t);
        result = function(directory, path, flags, mode);
    } else {
        result = function(directory, path, flags);
    }
    return retry_openat(function, directory, path, flags, result);
}

__attribute__((visibility("default"))) int openat(int directory,
        const char *path, int flags, ...) {
    va_list arguments;
    int result;

    va_start(arguments, flags);
    result = call_openat(real_openat, directory, path, flags, arguments);
    va_end(arguments);
    return result;
}

__attribute__((visibility("default"))) int openat64(int directory,
        const char *path, int flags, ...) {
    va_list arguments;
    int result;

    va_start(arguments, flags);
    result = call_openat(real_openat64, directory, path, flags, arguments);
    va_end(arguments);
    return result;
}

static int call_checked_openat(openat_checked_function checked_function,
        openat_function fallback_function, int directory, const char *path,
        int flags) {
    int result;

    if (checked_function == NULL) {
        errno = ENOSYS;
        return -1;
    }
    result = checked_function(directory, path, flags);
    return retry_openat(fallback_function, directory, path, flags, result);
}

__attribute__((visibility("default"))) int __openat_2(int directory,
        const char *path, int flags) {
    return call_checked_openat(real_openat_2, real_openat, directory, path,
        flags);
}

__attribute__((visibility("default"))) int __openat64_2(int directory,
        const char *path, int flags) {
    return call_checked_openat(real_openat64_2, real_openat64, directory, path,
        flags);
}
