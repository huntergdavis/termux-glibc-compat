#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

enum { EXIT_UNSUPPORTED = 77 };

struct payload {
    uint32_t parent_value;
    uint32_t child_value;
};

int main(void)
{
    int shmid = shmget(IPC_PRIVATE, sizeof(struct payload), IPC_CREAT | 0600);
    if (shmid == -1 && (errno == ENOSYS || errno == EPERM)) {
        printf("SysV shared memory unsupported: errno=%d\n", errno);
        return EXIT_UNSUPPORTED;
    }
    if (shmid == -1) {
        perror("shmget");
        return EXIT_FAILURE;
    }

    struct payload *data = shmat(shmid, NULL, 0);
    if (data == (void *)-1) {
        perror("shmat");
        (void)shmctl(shmid, IPC_RMID, NULL);
        return EXIT_FAILURE;
    }
    data->parent_value = UINT32_C(0x13579bdf);
    data->child_value = 0;

    pid_t child = fork();
    if (child == -1) {
        perror("fork");
        (void)shmdt(data);
        (void)shmctl(shmid, IPC_RMID, NULL);
        return EXIT_FAILURE;
    }
    if (child == 0) {
        if (data->parent_value != UINT32_C(0x13579bdf)) {
            _exit(2);
        }
        data->child_value = UINT32_C(0x2468ace0);
        _exit(0);
    }

    int status = 0;
    int failed = waitpid(child, &status, 0) != child ||
                 !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
                 data->child_value != UINT32_C(0x2468ace0);

    if (shmdt(data) == -1) {
        perror("shmdt");
        failed = 1;
    }
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        perror("shmctl(IPC_RMID)");
        failed = 1;
    }

    printf("SysV shared memory cross-process transfer: %s\n",
           failed ? "failed" : "ok");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
