#define _GNU_SOURCE

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "robust-shim check failed at line %d: %s\n", \
            __LINE__, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

struct thread_result {
    uintptr_t head_address;
};

static void check_head(struct robust_list_head *head, size_t length) {
    struct robust_list *const *previous;

    CHECK(head != NULL);
    CHECK(length == sizeof(*head));
    previous = (struct robust_list *const *)((const char *)head -
        sizeof(*previous));
    CHECK(*previous == &head->list);
    CHECK(head->list.next == &head->list);
    CHECK(head->futex_offset == -32);
    CHECK(head->list_op_pending == NULL);
}

static void *query_from_thread(void *argument) {
    struct thread_result *result = argument;
    struct robust_list_head *head = NULL;
    size_t length = 0;

    CHECK(syscall(SYS_get_robust_list, 0L, &head, &length) == 0);
    check_head(head, length);
    result->head_address = (uintptr_t)head;
    return NULL;
}

static void reexecute_with_shim(const char *shim) {
    CHECK(shim[0] == '/');
    CHECK(setenv("TGCOMPAT_ROBUST_LIST", "1", 1) == 0);
    CHECK(setenv("TGCOMPAT_USERFAULTFD_ENOSYS", "1", 1) == 0);
    CHECK(setenv("TGCOMPAT_ROBUST_SHIM_TEST", "1", 1) == 0);
    CHECK(setenv("LD_PRELOAD", shim, 1) == 0);
    execl("/proc/self/exe", "test-robust-shim", shim, (char *)NULL);
    perror("robust-shim re-exec");
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    struct robust_list_head *head = NULL;
    struct robust_list_head *second_head = NULL;
    struct thread_result thread_result = {0};
    pthread_t thread;
    size_t length = 0;
    size_t second_length = 0;
    long process;

    CHECK(argc == 2);
    if (getenv("TGCOMPAT_ROBUST_SHIM_TEST") == NULL) {
        reexecute_with_shim(argv[1]);
    }

    _Static_assert(sizeof(struct robust_list_head) == 24,
        "Steam requires the 64-bit Linux robust-list ABI");
    process = syscall(SYS_getpid, 0L, 0L, 0L, 0L, 0L, 0L);
    CHECK(process == (long)getpid());

    errno = 0;
    CHECK(syscall(374L, 0x80801L, 0L, 0L, 0L, 0L, 0L) == -1);
    CHECK(errno == ENOSYS);
#ifdef SYS_userfaultfd
    errno = 0;
    CHECK(syscall(SYS_userfaultfd, 0L, 0L, 0L, 0L, 0L, 0L) == -1);
    CHECK(errno == ENOSYS);
#endif

    errno = EBUSY;
    CHECK(syscall(SYS_get_robust_list, 0L, &head, &length) == 0);
    CHECK(errno == EBUSY);
    check_head(head, length);

    CHECK(syscall(SYS_get_robust_list, 0L, &second_head,
        &second_length) == 0);
    CHECK(second_head == head);
    check_head(second_head, second_length);

    CHECK(pthread_create(&thread, NULL, query_from_thread,
        &thread_result) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(thread_result.head_address != 0);
    CHECK(thread_result.head_address != (uintptr_t)head);

    errno = 0;
    CHECK(syscall(SYS_get_robust_list, 0L, NULL, &length) == -1);
    CHECK(errno == EFAULT);

    puts("robust-list syscall shim: PASS");
    return EXIT_SUCCESS;
}
