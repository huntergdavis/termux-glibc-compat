#define _GNU_SOURCE

#include <tgcompat/client.h>
#include <tgcompat/sem_store.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "temp-path.h"

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            failed = 1;                                                         \
            goto done;                                                          \
        }                                                                       \
    } while (0)

static int wait_for_broker(struct tgc_client *client)
{
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
    for (int attempt = 0; attempt < 200; ++attempt) {
        int result = tgc_client_ping(client);
        if (result == 0) {
            return 0;
        }
        if (result != -ENOENT && result != -ECONNREFUSED) {
            return result;
        }
        (void)nanosleep(&pause, NULL);
    }
    return -ETIMEDOUT;
}

int main(void)
{
    int failed = 0;
    char directory[TGC_TEST_PATH_CAPACITY];
    char *created_directory = NULL;
    char socket_path[sizeof(directory) + 24];
    pid_t daemon_pid = -1;
    pid_t child_pid = -1;
    struct tgc_client client;
    int client_initialized = 0;
    CHECK(tgc_test_temp_template(directory, sizeof(directory),
                                 "tgcompat-client") == 0);
    created_directory = mkdtemp(directory);
    CHECK(created_directory != NULL);
    int written = snprintf(socket_path, sizeof(socket_path), "%s/broker.sock",
                           created_directory);
    CHECK(written > 0 && (size_t)written < sizeof(socket_path));
    CHECK(tgc_client_init(&client, socket_path) == 0);
    client_initialized = 1;

    daemon_pid = fork();
    CHECK(daemon_pid >= 0);
    if (daemon_pid == 0) {
        execl("./build/tgcompatd", "tgcompatd", "--socket", socket_path,
              (char *)NULL);
        _exit(127);
    }
    CHECK(wait_for_broker(&client) == 0);

    int semid = tgc_client_semget(&client, 9876, 2, TGC_IPC_CREAT | 0600);
    CHECK(semid > 0);
    struct tgc_sem_metadata metadata;
    CHECK(tgc_client_stat(&client, semid, &metadata) == 0);
    CHECK(metadata.key == 9876 && metadata.uid == geteuid() &&
          metadata.gid == getegid() && metadata.mode == 0600 &&
          metadata.nsems == 2 && metadata.otime == 0);
    struct tgc_sem_metadata indexed_metadata;
    CHECK(tgc_client_stat_index(&client, 0, &indexed_metadata) == semid);
    CHECK(indexed_metadata.key == 9876 && indexed_metadata.nsems == 2);
    CHECK(tgc_client_stat_index(&client, TGC_SEM_MAX_SETS,
                                &indexed_metadata) == -EINVAL);
    CHECK(tgc_client_set_metadata(&client, semid, geteuid(), getegid(), 0640) ==
          0);
    CHECK(tgc_client_stat(&client, semid, &metadata) == 0);
    CHECK(metadata.mode == 0640);
    struct tgc_sem_info info;
    CHECK(tgc_client_info(&client, 0, &info) >= 0);
    CHECK(info.semmni == TGC_SEM_MAX_SETS &&
          info.semmsl == TGC_SEM_MAX_PER_SET &&
          info.semvmx == TGC_SEM_MAX_VALUE);
    CHECK(tgc_client_info(&client, 1, &info) >= 0);
    CHECK(info.semusz == 1 && info.semaem == 2);
    uint16_t set_values[] = {3, 4};
    CHECK(tgc_client_setall(&client, semid, set_values, 2) == 0);
    uint16_t get_values[] = {0, 0};
    CHECK(tgc_client_getall(&client, semid, get_values, 2) == 0);
    CHECK(get_values[0] == 3 && get_values[1] == 4);

    const struct tgc_sem_op operations[] = {
        {.sem_num = 0, .sem_op = -2, .sem_flg = 0},
        {.sem_num = 1, .sem_op = 2, .sem_flg = 0},
    };
    CHECK(tgc_client_semop(&client, semid, operations, 2) == 0);
    CHECK(tgc_client_getval(&client, semid, 0) == 1);
    CHECK(tgc_client_getval(&client, semid, 1) == 6);

    const struct tgc_sem_op blocked_nowait = {
        .sem_num = 0,
        .sem_op = -2,
        .sem_flg = TGC_IPC_NOWAIT,
    };
    CHECK(tgc_client_semop(&client, semid, &blocked_nowait, 1) == -EAGAIN);
    CHECK(tgc_client_getncnt(&client, semid, 0) == 0);
    CHECK(tgc_client_getzcnt(&client, semid, 0) == 0);
    CHECK(tgc_client_semtimedop(&client, semid, &blocked_nowait, 1,
                                1000000) == -EAGAIN);
    const struct tgc_sem_op blocked_timed = {
        .sem_num = 0,
        .sem_op = -2,
        .sem_flg = 0,
    };
    CHECK(tgc_client_semtimedop(&client, semid, &blocked_timed, 1,
                                20000000) == -EAGAIN);

    child_pid = fork();
    CHECK(child_pid >= 0);
    if (child_pid == 0) {
        /* The copied client owns the parent's descriptor. Its PID check must
         * close that copy and establish fresh SO_PEERCRED before mutation. */
        int result = tgc_client_setval(&client, semid, 1, 8);
        _exit(result == 0 ? 0 : 20);
    }
    int child_status = 0;
    CHECK(waitpid(child_pid, &child_status, 0) == child_pid);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    CHECK(tgc_client_getval(&client, semid, 1) == 8);
    CHECK(tgc_client_getpid(&client, semid, 1) == child_pid);
    child_pid = -1;

    CHECK(tgc_client_setval(&client, semid, 0, 0) == 0);
    child_pid = fork();
    CHECK(child_pid >= 0);
    if (child_pid == 0) {
        const struct tgc_sem_op undo_increment = {
            .sem_num = 0,
            .sem_op = 1,
            .sem_flg = TGC_SEM_UNDO,
        };
        int result = tgc_client_semop(&client, semid, &undo_increment, 1);
        if (result != 0 || tgc_client_getval(&client, semid, 0) != 1) {
            _exit(21);
        }
        _exit(0);
    }
    CHECK(waitpid(child_pid, &child_status, 0) == child_pid);
    CHECK(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);
    child_pid = -1;
    int restored_value = -1;
    for (int attempt = 0; attempt < 200; ++attempt) {
        restored_value = tgc_client_getval(&client, semid, 0);
        if (restored_value == 0) {
            break;
        }
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
        (void)nanosleep(&pause, NULL);
    }
    CHECK(restored_value == 0);

    CHECK(tgc_client_remove(&client, semid) == 0);
    CHECK(tgc_client_getval(&client, semid, 0) == -EINVAL);

done:
    if (child_pid > 0) {
        (void)kill(child_pid, SIGKILL);
        (void)waitpid(child_pid, NULL, 0);
    }
    if (client_initialized != 0) {
        tgc_client_close(&client);
    }
    if (daemon_pid > 0) {
        int status = 0;
        (void)kill(daemon_pid, SIGTERM);
        if (waitpid(daemon_pid, &status, 0) != daemon_pid ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed = 1;
        }
    }
    if (created_directory != NULL) {
        (void)unlink(socket_path);
        if (rmdir(created_directory) != 0) {
            failed = 1;
        }
    }
    if (!failed) {
        puts("client: PASS");
    }
    return failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
