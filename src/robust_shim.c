#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <linux/futex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

typedef long (*syscall_function)(long, ...);

static syscall_function real_syscall;
static bool robust_list_enabled;
static _Thread_local struct robust_list_head synthetic_head;
static _Thread_local bool synthetic_head_initialized;

static void resolve_syscall(void) {
    void *symbol = dlsym(RTLD_NEXT, "syscall");

    _Static_assert(sizeof(real_syscall) == sizeof(symbol),
        "function and data pointers must have equal size");
    memcpy(&real_syscall, &symbol, sizeof(real_syscall));
}

__attribute__((constructor)) static void initialize_robust_shim(void) {
    const char *value = getenv("TGCOMPAT_ROBUST_LIST");

    robust_list_enabled = value != NULL && strcmp(value, "1") == 0;
    resolve_syscall();
}

static struct robust_list_head *current_synthetic_head(void) {
    if (!synthetic_head_initialized) {
        synthetic_head.list.next = &synthetic_head.list;
        synthetic_head.futex_offset = -32;
        synthetic_head.list_op_pending = NULL;
        synthetic_head_initialized = true;
    }
    return &synthetic_head;
}

static long emulate_get_robust_list(va_list arguments) {
    long process = va_arg(arguments, long);
    struct robust_list_head **head =
        va_arg(arguments, struct robust_list_head **);
    size_t *length = va_arg(arguments, size_t *);
    int saved_errno = errno;

    if (process != 0) {
        return 1;
    }
    if (head == NULL || length == NULL) {
        errno = EFAULT;
        return -1;
    }
    *head = current_synthetic_head();
    *length = sizeof(**head);
    errno = saved_errno;
    return 0;
}

static long forward_syscall(long number, va_list arguments) {
    long argument1 = va_arg(arguments, long);
    long argument2 = va_arg(arguments, long);
    long argument3 = va_arg(arguments, long);
    long argument4 = va_arg(arguments, long);
    long argument5 = va_arg(arguments, long);
    long argument6 = va_arg(arguments, long);

    if (real_syscall == NULL) {
        errno = ENOSYS;
        return -1;
    }
    return real_syscall(number, argument1, argument2, argument3, argument4,
        argument5, argument6);
}

__attribute__((visibility("default"))) long syscall(long number, ...) {
    va_list arguments;
    long result;

    va_start(arguments, number);
    if (robust_list_enabled && number == SYS_get_robust_list) {
        va_list emulation_arguments;

        va_copy(emulation_arguments, arguments);
        result = emulate_get_robust_list(emulation_arguments);
        va_end(emulation_arguments);
        if (result != 1) {
            va_end(arguments);
            return result;
        }
    }
    result = forward_syscall(number, arguments);
    va_end(arguments);
    return result;
}
