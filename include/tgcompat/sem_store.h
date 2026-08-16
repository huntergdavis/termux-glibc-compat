#ifndef TGCOMPAT_SEM_STORE_H
#define TGCOMPAT_SEM_STORE_H

#include <stddef.h>
#include <stdint.h>

enum {
    TGC_IPC_PRIVATE = 0,
    TGC_IPC_CREAT = 01000,
    TGC_IPC_EXCL = 02000,
    TGC_IPC_NOWAIT = 04000,
    TGC_SEM_UNDO = 010000,
    TGC_SEM_MAX_SETS = 128,
    TGC_SEM_MAX_PER_SET = 512,
    TGC_SEM_MAX_VALUE = 32767,
    TGC_SEM_OP_BLOCKED = 1,
};

struct tgc_sem_op {
    uint16_t sem_num;
    int16_t sem_op;
    uint16_t sem_flg;
};

struct tgc_sem_store;

struct tgc_sem_store *tgc_sem_store_create(void);
void tgc_sem_store_destroy(struct tgc_sem_store *store);

/*
 * Successful operations return a non-negative value. Failures return the
 * negative errno value so the future glibc client can translate it at one
 * boundary.
 */
int tgc_sem_store_get(struct tgc_sem_store *store, int32_t key, int nsems,
                      int flags);
int tgc_sem_store_remove(struct tgc_sem_store *store, int semid);

int tgc_sem_store_getval(const struct tgc_sem_store *store, int semid,
                         size_t semnum);
int tgc_sem_store_setval(struct tgc_sem_store *store, int semid,
                         size_t semnum, unsigned int value, int32_t pid);
int tgc_sem_store_getpid(const struct tgc_sem_store *store, int semid,
                         size_t semnum);
int tgc_sem_store_getall(const struct tgc_sem_store *store, int semid,
                         uint16_t *values, size_t count);
int tgc_sem_store_setall(struct tgc_sem_store *store, int semid,
                         const uint16_t *values, size_t count, int32_t pid);

/*
 * Applies every operation atomically. TGC_SEM_OP_BLOCKED means the caller may
 * queue the unchanged operation and retry it after a state change. A blocked
 * operation carrying TGC_IPC_NOWAIT returns -EAGAIN instead. SEM_UNDO is
 * rejected with -ENOTSUP until per-process undo tracking is implemented.
 */
int tgc_sem_store_tryop(struct tgc_sem_store *store, int semid,
                        const struct tgc_sem_op *operations, size_t count,
                        int32_t pid);

#endif
