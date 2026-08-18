#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__aarch64__)
_Static_assert(SYS_mmap == 222, "AArch64 mmap syscall number changed");
_Static_assert(SYS_munmap == 215, "AArch64 munmap syscall number changed");
_Static_assert(SYS_mprotect == 226,
    "AArch64 mprotect syscall number changed");
#endif

static long raw_syscall6(long number, long argument0, long argument1,
        long argument2, long argument3, long argument4, long argument5) {
#if defined(__aarch64__)
    register long x0 __asm__("x0") = argument0;
    register long x1 __asm__("x1") = argument1;
    register long x2 __asm__("x2") = argument2;
    register long x3 __asm__("x3") = argument3;
    register long x4 __asm__("x4") = argument4;
    register long x5 __asm__("x5") = argument5;
    register long x8 __asm__("x8") = number;

    __asm__ volatile("svc 0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
        : "memory", "cc");
    return x0;
#else
    return syscall(number, argument0, argument1, argument2, argument3,
        argument4, argument5);
#endif
}

static bool kernel_result_is_error(long result) {
    return (unsigned long)result >= (unsigned long)-4095L;
}

static int raw_mprotect(void *address, size_t length, int protection) {
    long result = raw_syscall6(SYS_mprotect, (long)(uintptr_t)address,
        (long)length, protection, 0, 0, 0);

    if (kernel_result_is_error(result)) {
        errno = (int)-result;
        return -1;
    }
    return (int)result;
}

static void *raw_mmap(void *address, size_t length, int protection,
        int flags, int descriptor, off_t offset) {
    long result = raw_syscall6(SYS_mmap, (long)(uintptr_t)address,
        (long)length, protection, flags, descriptor, (long)offset);

    if (kernel_result_is_error(result)) {
        errno = (int)-result;
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)result;
}

static int raw_munmap(void *address, size_t length) {
    long result = raw_syscall6(SYS_munmap, (long)(uintptr_t)address,
        (long)length, 0, 0, 0, 0);

    if (kernel_result_is_error(result)) {
        errno = (int)-result;
        return -1;
    }
    return (int)result;
}

static void copy_bytes(unsigned char *destination,
        const unsigned char *source, size_t length) {
    size_t index;

    for (index = 0; index < length; ++index) {
        destination[index] = source[index];
    }
}

/* Android can reject PROT_EXEC for mappings backed by removable/noexec
 * storage. Replace only the rejected range with an anonymous copy, which is
 * executable under the Termux app domain. This also avoids the allocator and
 * invalid-free bugs in the glibc-packages /proc/self/maps fallback. */
static int make_anonymous_executable_copy(void *address, size_t length,
        int protection) {
    void *scratch;
    void *replacement;
    int saved_errno;
    int readable_protection = PROT_READ | (protection & PROT_WRITE);

    if (length == 0) {
        errno = EINVAL;
        return -1;
    }
    if (raw_mprotect(address, length, readable_protection) != 0) {
        return -1;
    }
    scratch = raw_mmap(NULL, length, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (scratch == MAP_FAILED) {
        return -1;
    }
    copy_bytes(scratch, address, length);

    replacement = raw_mmap(address, length, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (replacement == MAP_FAILED) {
        saved_errno = errno;
        (void)raw_munmap(scratch, length);
        errno = saved_errno;
        return -1;
    }
    copy_bytes(address, scratch, length);
    if (raw_mprotect(address, length, protection) != 0) {
        saved_errno = errno;
        (void)raw_munmap(scratch, length);
        errno = saved_errno;
        return -1;
    }
    saved_errno = errno;
    (void)raw_munmap(scratch, length);
    errno = saved_errno;
    return 0;
}

__attribute__((visibility("default"))) int mprotect(void *address,
        size_t length, int protection) {
    int result;

#if defined(TGCOMPAT_MPROTECT_TEST_FORCE_EACCES)
    static bool force_initial_eacces = true;

    if (force_initial_eacces) {
        force_initial_eacces = false;
        errno = EACCES;
        result = -1;
    } else {
        result = raw_mprotect(address, length, protection);
    }
#else
    result = raw_mprotect(address, length, protection);
#endif
    if (result == -1 && errno == EACCES &&
            (protection & PROT_EXEC) != 0) {
        return make_anonymous_executable_copy(address, length, protection);
    }
    return result;
}

__attribute__((visibility("default"))) int __mprotect(void *address,
        size_t length, int protection) {
    return mprotect(address, length, protection);
}
