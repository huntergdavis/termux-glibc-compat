#ifndef TGCOMPAT_CLIENT_H
#define TGCOMPAT_CLIENT_H

#include <tgcompat/sem_store.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/un.h>

/*
 * One client instance is intentionally not shared between threads. Keep one
 * lazily connected instance per calling thread for a lock-free client hot
 * path. The owner PID check automatically reconnects after fork.
 */
struct tgc_client {
    int socket_fd;
    pid_t connection_pid;
    uint32_t next_request_id;
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

int tgc_client_init(struct tgc_client *client, const char *socket_path);
void tgc_client_close(struct tgc_client *client);

int tgc_client_ping(struct tgc_client *client);
int tgc_client_semget(struct tgc_client *client, int32_t key, int nsems,
                      int flags);
int tgc_client_remove(struct tgc_client *client, int semid);
int tgc_client_getval(struct tgc_client *client, int semid, size_t semnum);
int tgc_client_setval(struct tgc_client *client, int semid, size_t semnum,
                      unsigned int value);
int tgc_client_getpid(struct tgc_client *client, int semid, size_t semnum);
int tgc_client_getall(struct tgc_client *client, int semid, uint16_t *values,
                      size_t count);
int tgc_client_setall(struct tgc_client *client, int semid,
                      const uint16_t *values, size_t count);
int tgc_client_semop(struct tgc_client *client, int semid,
                     const struct tgc_sem_op *operations, size_t count);

#endif
