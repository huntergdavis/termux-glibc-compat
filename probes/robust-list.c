#define _GNU_SOURCE

#include <errno.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { EXIT_UNSUPPORTED = 77 };

int main(void)
{
    struct robust_list_head *head = NULL;
    size_t len = 0;
    errno = 0;
    long rc = syscall(SYS_get_robust_list, 0, &head, &len);

    if (rc == -1 && (errno == ENOSYS || errno == EPERM)) {
        printf("get_robust_list unsupported: errno=%d\n", errno);
        return EXIT_UNSUPPORTED;
    }
    if (rc != 0) {
        perror("get_robust_list");
        return EXIT_FAILURE;
    }

    printf("get_robust_list head=%p len=%zu expected=%zu\n",
           (void *)head, len, sizeof(*head));
    return head != NULL && len == sizeof(*head) ? EXIT_SUCCESS : EXIT_FAILURE;
}
