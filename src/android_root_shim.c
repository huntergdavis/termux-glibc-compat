#define _GNU_SOURCE

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "tgcompat/android_root.h"

typedef int (*open_function)(const char *, int, ...);
typedef int (*openat_function)(int, const char *, int, ...);
typedef int (*open_checked_function)(const char *, int);
typedef int (*openat_checked_function)(int, const char *, int);
typedef FILE *(*fopen_function)(const char *, const char *);
typedef DIR *(*opendir_function)(const char *);
typedef int (*access_function)(const char *, int);
typedef int (*stat_function)(const char *, struct stat *);

static open_function real_open;
static open_function real_open64;
static openat_function real_openat;
static openat_function real_openat64;
static open_checked_function real_open_2;
static open_checked_function real_open64_2;
static openat_checked_function real_openat_2;
static openat_checked_function real_openat64_2;
static fopen_function real_fopen;
static fopen_function real_fopen64;
static opendir_function real_opendir;
static access_function real_access;
static stat_function real_stat;
static stat_function real_lstat;

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

    symbol = next_symbol("open");
    resolve_function(symbol, &real_open, sizeof(real_open));
    symbol = next_symbol("open64");
    resolve_function(symbol, &real_open64, sizeof(real_open64));
    symbol = next_symbol("openat");
    resolve_function(symbol, &real_openat, sizeof(real_openat));
    symbol = next_symbol("openat64");
    resolve_function(symbol, &real_openat64, sizeof(real_openat64));
    symbol = next_symbol("__open_2");
    resolve_function(symbol, &real_open_2, sizeof(real_open_2));
    symbol = next_symbol("__open64_2");
    resolve_function(symbol, &real_open64_2, sizeof(real_open64_2));
    symbol = next_symbol("__openat_2");
    resolve_function(symbol, &real_openat_2, sizeof(real_openat_2));
    symbol = next_symbol("__openat64_2");
    resolve_function(symbol, &real_openat64_2, sizeof(real_openat64_2));
    symbol = next_symbol("fopen");
    resolve_function(symbol, &real_fopen, sizeof(real_fopen));
    symbol = next_symbol("fopen64");
    resolve_function(symbol, &real_fopen64, sizeof(real_fopen64));
    symbol = next_symbol("opendir");
    resolve_function(symbol, &real_opendir, sizeof(real_opendir));
    symbol = next_symbol("access");
    resolve_function(symbol, &real_access, sizeof(real_access));
    symbol = next_symbol("stat");
    resolve_function(symbol, &real_stat, sizeof(real_stat));
    symbol = next_symbol("lstat");
    resolve_function(symbol, &real_lstat, sizeof(real_lstat));
}

static const char *proc_net_suffix(const char *path) {
    static const char *const prefixes[] = {
        "/proc/net",
        "/proc/self/net",
        "/proc/thread-self/net",
    };
    size_t index;

    if (path == NULL) {
        return NULL;
    }
    for (index = 0; index < sizeof(prefixes) / sizeof(prefixes[0]); index++) {
        size_t length = strlen(prefixes[index]);

        if (strcmp(path, prefixes[index]) == 0) {
            return path + length;
        }
        if (strncmp(path, prefixes[index], length) == 0 &&
                path[length] == '/') {
            return path + length;
        }
    }
    return NULL;
}

const char *tgcompat_android_root_rewrite_proc_net(const char *path,
        char *output, size_t output_size) {
    const char *root;
    const char *suffix;
    size_t root_length;
    size_t suffix_length;

    suffix = proc_net_suffix(path);
    if (suffix == NULL) {
        return path;
    }
    root = getenv("TGCOMPAT_PROC_NET");
    if (root == NULL || root[0] != '/') {
        return path;
    }
    root_length = strlen(root);
    if (root_length < 2 || root[root_length - 1] == '/') {
        return path;
    }
    suffix_length = strlen(suffix);
    if (output == NULL || output_size == 0 ||
            suffix_length >= output_size ||
            root_length > output_size - suffix_length - 1) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(output, root, root_length);
    memcpy(output + root_length, suffix, suffix_length + 1);
    return output;
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

static bool read_only_open(int flags) {
    return (flags & O_ACCMODE) == O_RDONLY && !flags_have_mode(flags);
}

static const char *mapped_open_path(const char *path, int flags,
        char output[PATH_MAX]) {
    if (!read_only_open(flags)) {
        return path;
    }
    return tgcompat_android_root_rewrite_proc_net(path, output, PATH_MAX);
}

static int call_open(open_function function, const char *path, int flags,
        va_list arguments) {
    char rewritten[PATH_MAX];
    const char *mapped = mapped_open_path(path, flags, rewritten);

    if (function == NULL || mapped == NULL) {
        errno = function == NULL ? ENOSYS : errno;
        return -1;
    }
    if (flags_have_mode(flags)) {
        mode_t mode = va_arg(arguments, mode_t);

        return function(mapped, flags, mode);
    }
    return function(mapped, flags);
}

__attribute__((visibility("default"))) int open(const char *path,
        int flags, ...) {
    va_list arguments;
    int result;

    va_start(arguments, flags);
    result = call_open(real_open, path, flags, arguments);
    va_end(arguments);
    return result;
}

__attribute__((visibility("default"))) int open64(const char *path,
        int flags, ...) {
    va_list arguments;
    int result;

    va_start(arguments, flags);
    result = call_open(real_open64, path, flags, arguments);
    va_end(arguments);
    return result;
}

static int call_openat(openat_function function, int directory,
        const char *path, int flags, va_list arguments) {
    char rewritten[PATH_MAX];
    const char *mapped = mapped_open_path(path, flags, rewritten);
    int result;

    if (function == NULL || mapped == NULL) {
        errno = function == NULL ? ENOSYS : errno;
        return -1;
    }
    if (flags_have_mode(flags)) {
        mode_t mode = va_arg(arguments, mode_t);
        result = function(directory, mapped, flags, mode);
    } else {
        result = function(directory, mapped, flags);
    }
    return retry_openat(function, directory, mapped, flags, result);
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

static int call_checked_open(open_checked_function checked_function,
        const char *path, int flags) {
    char rewritten[PATH_MAX];
    const char *mapped = mapped_open_path(path, flags, rewritten);

    if (checked_function == NULL || mapped == NULL) {
        errno = checked_function == NULL ? ENOSYS : errno;
        return -1;
    }
    return checked_function(mapped, flags);
}

__attribute__((visibility("default"))) int __open_2(const char *path,
        int flags) {
    return call_checked_open(real_open_2, path, flags);
}

__attribute__((visibility("default"))) int __open64_2(const char *path,
        int flags) {
    return call_checked_open(real_open64_2, path, flags);
}

static int call_checked_openat(openat_checked_function checked_function,
        openat_function fallback_function, int directory, const char *path,
        int flags) {
    char rewritten[PATH_MAX];
    const char *mapped = mapped_open_path(path, flags, rewritten);
    int result;

    if (checked_function == NULL || mapped == NULL) {
        errno = checked_function == NULL ? ENOSYS : errno;
        return -1;
    }
    result = checked_function(directory, mapped, flags);
    return retry_openat(fallback_function, directory, mapped, flags, result);
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

static bool read_only_fopen_mode(const char *mode) {
    return mode != NULL && mode[0] == 'r' && strchr(mode, '+') == NULL;
}

static FILE *call_fopen(fopen_function function, const char *path,
        const char *mode) {
    char rewritten[PATH_MAX];
    const char *mapped = path;

    if (read_only_fopen_mode(mode)) {
        mapped = tgcompat_android_root_rewrite_proc_net(path, rewritten,
            sizeof(rewritten));
    }
    if (function == NULL || mapped == NULL) {
        errno = function == NULL ? ENOSYS : errno;
        return NULL;
    }
    return function(mapped, mode);
}

__attribute__((visibility("default"))) FILE *fopen(const char *path,
        const char *mode) {
    return call_fopen(real_fopen, path, mode);
}

__attribute__((visibility("default"))) FILE *fopen64(const char *path,
        const char *mode) {
    return call_fopen(real_fopen64, path, mode);
}

__attribute__((visibility("default"))) DIR *opendir(const char *path) {
    char rewritten[PATH_MAX];
    const char *mapped = tgcompat_android_root_rewrite_proc_net(path,
        rewritten, sizeof(rewritten));

    if (real_opendir == NULL || mapped == NULL) {
        errno = real_opendir == NULL ? ENOSYS : errno;
        return NULL;
    }
    return real_opendir(mapped);
}

__attribute__((visibility("default"))) int access(const char *path,
        int mode) {
    char rewritten[PATH_MAX];
    const char *mapped = path;

    if ((mode & W_OK) == 0) {
        mapped = tgcompat_android_root_rewrite_proc_net(path, rewritten,
            sizeof(rewritten));
    }
    if (real_access == NULL || mapped == NULL) {
        errno = real_access == NULL ? ENOSYS : errno;
        return -1;
    }
    return real_access(mapped, mode);
}

static int call_stat(stat_function function, const char *path,
        struct stat *buffer) {
    char rewritten[PATH_MAX];
    const char *mapped = tgcompat_android_root_rewrite_proc_net(path,
        rewritten, sizeof(rewritten));

    if (function == NULL || mapped == NULL) {
        errno = function == NULL ? ENOSYS : errno;
        return -1;
    }
    return function(mapped, buffer);
}

__attribute__((visibility("default"))) int stat(const char *path,
        struct stat *buffer) {
    return call_stat(real_stat, path, buffer);
}

__attribute__((visibility("default"))) int lstat(const char *path,
        struct stat *buffer) {
    return call_stat(real_lstat, path, buffer);
}
