#define _GNU_SOURCE

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { EXIT_UNSUPPORTED = 77 };

union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
    struct seminfo *__buf;
};

static void pause_milliseconds(long milliseconds)
{
    struct timespec request = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000L,
    };
    while (nanosleep(&request, &request) == -1 && errno == EINTR) {
    }
}

static int wait_for_child(pid_t child, int *status)
{
    for (int i = 0; i < 500; ++i) {
        pid_t rc = waitpid(child, status, WNOHANG);
        if (rc == child) {
            return 0;
        }
        if (rc == -1) {
            return -1;
        }
        pause_milliseconds(10);
    }
    return 1;
}

int main(void)
{
    int semid = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
    if (semid == -1 && (errno == ENOSYS || errno == EPERM)) {
        printf("SysV semaphores unsupported: errno=%d\n", errno);
        return EXIT_UNSUPPORTED;
    }
    if (semid == -1) {
        perror("semget");
        return EXIT_FAILURE;
    }

    int failed = 0;
    unsigned short initial[2] = {0, 1};
    union semun argument = {.array = initial};
    if (semctl(semid, 0, SETALL, argument) == -1) {
        perror("semctl(SETALL)");
        failed = 1;
        goto cleanup;
    }

    unsigned short observed[2] = {0, 0};
    argument.array = observed;
    if (semctl(semid, 0, GETALL, argument) == -1 ||
        observed[0] != 0 || observed[1] != 1) {
        perror("semctl(GETALL)");
        failed = 1;
        goto cleanup;
    }

    struct semid_ds metadata;
    argument.buf = &metadata;
    if (semctl(semid, 0, IPC_STAT, argument) == -1 ||
        metadata.sem_nsems != 2 || (metadata.sem_perm.mode & 0777U) != 0600U) {
        perror("semctl(IPC_STAT)");
        failed = 1;
        goto cleanup;
    }
    metadata.sem_perm.mode =
        (metadata.sem_perm.mode & ~(unsigned int)0777U) | 0640U;
    if (semctl(semid, 0, IPC_SET, argument) == -1) {
        perror("semctl(IPC_SET)");
        failed = 1;
        goto cleanup;
    }
    if (semctl(semid, 0, IPC_STAT, argument) == -1 ||
        (metadata.sem_perm.mode & 0777U) != 0640U) {
        perror("semctl(IPC_STAT after IPC_SET)");
        failed = 1;
        goto cleanup;
    }

    struct seminfo info;
    argument.__buf = &info;
    int highest_index = semctl(0, 0, IPC_INFO, argument);
    if (highest_index == -1 || info.semmni <= 0 ||
        info.semmsl < 2 || info.semvmx < 1) {
        perror("semctl(IPC_INFO)");
        failed = 1;
        goto cleanup;
    }
    struct semid_ds indexed_metadata;
    argument.buf = &indexed_metadata;
    if (semctl(highest_index, 0, SEM_STAT_ANY, argument) == -1 ||
        indexed_metadata.sem_nsems == 0) {
        perror("semctl(SEM_STAT_ANY)");
        failed = 1;
        goto cleanup;
    }

    struct sembuf timed_decrement = {
        .sem_num = 0,
        .sem_op = -1,
        .sem_flg = 0,
    };
    const struct timespec timeout = {.tv_sec = 0, .tv_nsec = 20000000L};
    errno = 0;
    if (semtimedop(semid, &timed_decrement, 1, &timeout) != -1 ||
        errno != EAGAIN) {
        fprintf(stderr, "semtimedop timeout mismatch: errno=%d\n", errno);
        failed = 1;
        goto cleanup;
    }

    int ready[2];
    if (pipe(ready) == -1) {
        perror("pipe");
        failed = 1;
        goto cleanup;
    }

    pid_t child = fork();
    if (child == -1) {
        perror("fork");
        (void)close(ready[0]);
        (void)close(ready[1]);
        failed = 1;
        goto cleanup;
    }
    if (child == 0) {
        (void)close(ready[0]);
        char marker = 'R';
        if (write(ready[1], &marker, 1) != 1) {
            _exit(2);
        }
        (void)close(ready[1]);
        alarm(5);
        struct sembuf decrement = {.sem_num = 0, .sem_op = -1, .sem_flg = 0};
        _exit(semop(semid, &decrement, 1) == 0 ? 0 : 3);
    }

    (void)close(ready[1]);
    char marker = 0;
    if (read(ready[0], &marker, 1) != 1 || marker != 'R') {
        fprintf(stderr, "child readiness handshake failed\n");
        failed = 1;
    }
    (void)close(ready[0]);

    int waiter_count = 0;
    for (int i = 0; i < 200 && !failed; ++i) {
        waiter_count = semctl(semid, 0, GETNCNT);
        if (waiter_count == 1) {
            break;
        }
        if (waiter_count == -1) {
            perror("semctl(GETNCNT)");
            failed = 1;
            break;
        }
        pause_milliseconds(10);
    }
    if (!failed && waiter_count != 1) {
        fprintf(stderr, "blocked waiter was not reported: count=%d\n", waiter_count);
        failed = 1;
    }

    argument.val = 1;
    if (!failed && semctl(semid, 0, SETVAL, argument) == -1) {
        perror("semctl(SETVAL)");
        failed = 1;
    }

    int status = 0;
    int wait_rc = wait_for_child(child, &status);
    if (wait_rc != 0) {
        fprintf(stderr, "blocked semop did not wake in time\n");
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
        failed = 1;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "child semop failed: status=%d\n", status);
        failed = 1;
    }

    if (!failed) {
        int last_pid = semctl(semid, 0, GETPID);
        int value = semctl(semid, 0, GETVAL);
        if (last_pid != child || value != 0) {
            fprintf(stderr,
                    "post-wakeup state mismatch: pid=%d expected=%ld value=%d\n",
                    last_pid, (long)child, value);
            failed = 1;
        }
    }

    pid_t undo_child = -1;
    if (!failed) {
        undo_child = fork();
        if (undo_child == -1) {
            perror("fork for SEM_UNDO");
            failed = 1;
        } else if (undo_child == 0) {
            struct sembuf increment = {
                .sem_num = 1,
                .sem_op = 1,
                .sem_flg = SEM_UNDO,
            };
            _exit(semop(semid, &increment, 1) == 0 ? 0 : 4);
        }
    }
    if (!failed) {
        int undo_status = 0;
        int undo_wait = wait_for_child(undo_child, &undo_status);
        if (undo_wait != 0 || !WIFEXITED(undo_status) ||
            WEXITSTATUS(undo_status) != 0) {
            fprintf(stderr, "SEM_UNDO child failed: status=%d\n", undo_status);
            if (undo_wait != 0) {
                (void)kill(undo_child, SIGKILL);
                (void)waitpid(undo_child, &undo_status, 0);
            }
            failed = 1;
        }
    }
    if (!failed) {
        int restored_value = -1;
        for (int i = 0; i < 200; ++i) {
            restored_value = semctl(semid, 1, GETVAL);
            if (restored_value == 1) {
                break;
            }
            pause_milliseconds(10);
        }
        if (restored_value != 1) {
            fprintf(stderr, "SEM_UNDO did not restore value: %d\n",
                    restored_value);
            failed = 1;
        }
    }

cleanup:
    if (semctl(semid, 0, IPC_RMID) == -1) {
        perror("semctl(IPC_RMID)");
        failed = 1;
    }
    printf("SysV semaphore control and wakeup: %s\n",
           failed ? "failed" : "ok");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
