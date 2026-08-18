#define _GNU_SOURCE

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tgcompat/android_root.h"

int main(void) {
    char proc_net_template[PATH_MAX];
    char proc_stat_path[PATH_MAX];
    char route_path[PATH_MAX];
    char rewritten[PATH_MAX];
    char line[64];
    const char *mapped;
    struct stat metadata;
    DIR *directory;
    FILE *stream;
    int descriptor;
    int retry_flags = -1;
    int written;
    int original = O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY;
    const char *temporary_root;

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

    descriptor = openat(AT_FDCWD, "/proc/self/root",
        O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
    assert(descriptor >= 0);
    assert(fstat(descriptor, &metadata) == 0);
    assert(S_ISDIR(metadata.st_mode));
    assert(close(descriptor) == 0);

    assert(unsetenv("TGCOMPAT_PROC_NET") == 0);
    mapped = tgcompat_android_root_rewrite_proc_net("/proc/net/route",
        rewritten, sizeof(rewritten));
    assert(mapped != NULL && strcmp(mapped, "/proc/net/route") == 0);
    temporary_root = getenv("TMPDIR");
    if (temporary_root == NULL || temporary_root[0] != '/') {
        temporary_root = "/tmp";
    }
    written = snprintf(proc_net_template, sizeof(proc_net_template),
        "%s/tgcompat-proc-net-XXXXXX", temporary_root);
    assert(written > 0 && (size_t)written < sizeof(proc_net_template));
    assert(mkdtemp(proc_net_template) == proc_net_template);
    written = snprintf(route_path, sizeof(route_path), "%s/route",
        proc_net_template);
    assert(written > 0 && (size_t)written < sizeof(route_path));
    stream = fopen(route_path, "w");
    assert(stream != NULL);
    assert(fputs("mapped-route\n", stream) >= 0);
    assert(fclose(stream) == 0);
    assert(setenv("TGCOMPAT_PROC_NET", proc_net_template, 1) == 0);

    written = snprintf(proc_stat_path, sizeof(proc_stat_path), "%s/proc-stat",
        proc_net_template);
    assert(written > 0 && (size_t)written < sizeof(proc_stat_path));
    stream = fopen(proc_stat_path, "w");
    assert(stream != NULL);
    assert(fputs("cpu  0 0 0 0 0 0 0 0 0 0\n", stream) >= 0);
    assert(fclose(stream) == 0);
    assert(unsetenv("TGCOMPAT_PROC_STAT") == 0);
    mapped = tgcompat_android_root_rewrite_proc_stat("/proc/stat",
        rewritten, sizeof(rewritten));
    assert(mapped != NULL && strcmp(mapped, "/proc/stat") == 0);
    assert(setenv("TGCOMPAT_PROC_STAT", proc_stat_path, 1) == 0);

    mapped = tgcompat_android_root_rewrite_proc_net("/proc/net/route",
        rewritten, sizeof(rewritten));
    assert(mapped == rewritten && strcmp(mapped, route_path) == 0);
    mapped = tgcompat_android_root_rewrite_proc_net("/proc/self/net/route",
        rewritten, sizeof(rewritten));
    assert(mapped == rewritten && strcmp(mapped, route_path) == 0);
    mapped = tgcompat_android_root_rewrite_proc_net(
        "/proc/thread-self/net/route", rewritten, sizeof(rewritten));
    assert(mapped == rewritten && strcmp(mapped, route_path) == 0);
    mapped = tgcompat_android_root_rewrite_proc_net("/proc/network/route",
        rewritten, sizeof(rewritten));
    assert(mapped != NULL && strcmp(mapped, "/proc/network/route") == 0);
    errno = 0;
    assert(tgcompat_android_root_rewrite_proc_net("/proc/net/route",
        rewritten, 2) == NULL);
    assert(errno == ENAMETOOLONG);

    mapped = tgcompat_android_root_rewrite_proc_stat("/proc/stat",
        rewritten, sizeof(rewritten));
    assert(mapped == rewritten && strcmp(mapped, proc_stat_path) == 0);
    mapped = tgcompat_android_root_rewrite_proc("/proc/stat", rewritten,
        sizeof(rewritten));
    assert(mapped == rewritten && strcmp(mapped, proc_stat_path) == 0);
    mapped = tgcompat_android_root_rewrite_proc_stat("/proc/self/stat",
        rewritten, sizeof(rewritten));
    assert(mapped != NULL && strcmp(mapped, "/proc/self/stat") == 0);
    errno = 0;
    assert(tgcompat_android_root_rewrite_proc_stat("/proc/stat", rewritten,
        2) == NULL);
    assert(errno == ENAMETOOLONG);

    stream = fopen("/proc/net/route", "r");
    assert(stream != NULL && fgets(line, sizeof(line), stream) == line);
    assert(strcmp(line, "mapped-route\n") == 0 && fclose(stream) == 0);
    stream = fopen64("/proc/self/net/route", "r");
    assert(stream != NULL && fgets(line, sizeof(line), stream) == line);
    assert(strcmp(line, "mapped-route\n") == 0 && fclose(stream) == 0);
    descriptor = open("/proc/net/route", O_RDONLY | O_CLOEXEC);
    assert(descriptor >= 0 && close(descriptor) == 0);
    descriptor = open64("/proc/self/net/route", O_RDONLY | O_CLOEXEC);
    assert(descriptor >= 0 && close(descriptor) == 0);
    descriptor = openat(AT_FDCWD, "/proc/thread-self/net/route",
        O_RDONLY | O_CLOEXEC);
    assert(descriptor >= 0 && close(descriptor) == 0);
    assert(access("/proc/net/route", R_OK) == 0);
    assert(stat("/proc/self/net/route", &metadata) == 0 &&
        metadata.st_size == 13);
    assert(lstat("/proc/thread-self/net/route", &metadata) == 0 &&
        metadata.st_size == 13);
    directory = opendir("/proc/net");
    assert(directory != NULL && closedir(directory) == 0);

    stream = fopen("/proc/stat", "r");
    assert(stream != NULL && fgets(line, sizeof(line), stream) == line);
    assert(strcmp(line, "cpu  0 0 0 0 0 0 0 0 0 0\n") == 0 &&
        fclose(stream) == 0);
    descriptor = open("/proc/stat", O_RDONLY | O_CLOEXEC);
    assert(descriptor >= 0 && close(descriptor) == 0);
    descriptor = openat(AT_FDCWD, "/proc/stat", O_RDONLY | O_CLOEXEC);
    assert(descriptor >= 0 && close(descriptor) == 0);
    assert(access("/proc/stat", R_OK) == 0);
    assert(stat("/proc/stat", &metadata) == 0 &&
        metadata.st_size ==
            (off_t)(sizeof("cpu  0 0 0 0 0 0 0 0 0 0\n") - 1));
    assert(lstat("/proc/stat", &metadata) == 0 &&
        metadata.st_size ==
            (off_t)(sizeof("cpu  0 0 0 0 0 0 0 0 0 0\n") - 1));

    assert(unsetenv("TGCOMPAT_PROC_NET") == 0);
    assert(unsetenv("TGCOMPAT_PROC_STAT") == 0);
    assert(unlink(proc_stat_path) == 0);
    assert(unlink(route_path) == 0);
    assert(rmdir(proc_net_template) == 0);

    puts("Android real-root and proc shadow shim policy: PASS");
    return 0;
}
