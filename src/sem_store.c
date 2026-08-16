#define _POSIX_C_SOURCE 200809L

#include <tgcompat/sem_store.h>

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    SEM_ID_INDEX_BITS = 16,
    SEM_ID_INDEX_MASK = 0xffff,
    SEM_ID_MAX_GENERATION = 0x7fff,
};

struct tgc_sem_set {
    int32_t key;
    uint16_t generation;
    uint16_t nsems;
    bool active;
    uint32_t uid;
    uint32_t gid;
    uint32_t cuid;
    uint32_t cgid;
    uint32_t mode;
    int64_t otime;
    int64_t ctime;
    uint16_t *values;
    int32_t *last_pids;
    uint32_t *negative_waiters;
    uint32_t *zero_waiters;
};

struct tgc_sem_store {
    struct tgc_sem_set sets[TGC_SEM_MAX_SETS];
};

static int make_id(size_t index, uint16_t generation)
{
    return (int)(((uint32_t)generation << SEM_ID_INDEX_BITS) | (index + 1));
}

static bool decode_id(int semid, size_t *index, uint16_t *generation)
{
    if (semid <= 0 || index == NULL || generation == NULL) {
        return false;
    }

    uint32_t id = (uint32_t)semid;
    uint32_t encoded_index = id & SEM_ID_INDEX_MASK;
    uint16_t encoded_generation = (uint16_t)(id >> SEM_ID_INDEX_BITS);
    if (encoded_index == 0 || encoded_index > TGC_SEM_MAX_SETS ||
        encoded_generation == 0) {
        return false;
    }

    *index = encoded_index - 1;
    *generation = encoded_generation;
    return true;
}

static struct tgc_sem_set *find_set(struct tgc_sem_store *store, int semid)
{
    size_t index = 0;
    uint16_t generation = 0;
    if (store == NULL || !decode_id(semid, &index, &generation)) {
        return NULL;
    }

    struct tgc_sem_set *set = &store->sets[index];
    return set->active && set->generation == generation ? set : NULL;
}

static const struct tgc_sem_set *find_const_set(
    const struct tgc_sem_store *store, int semid)
{
    size_t index = 0;
    uint16_t generation = 0;
    if (store == NULL || !decode_id(semid, &index, &generation)) {
        return NULL;
    }

    const struct tgc_sem_set *set = &store->sets[index];
    return set->active && set->generation == generation ? set : NULL;
}

static uint16_t next_generation(uint16_t generation)
{
    return generation >= SEM_ID_MAX_GENERATION ? 1 : (uint16_t)(generation + 1);
}

static int64_t current_time_seconds(void)
{
    struct timespec now;
    return clock_gettime(CLOCK_REALTIME, &now) == 0 ? (int64_t)now.tv_sec : 0;
}

struct tgc_sem_store *tgc_sem_store_create(void)
{
    return calloc(1, sizeof(struct tgc_sem_store));
}

void tgc_sem_store_destroy(struct tgc_sem_store *store)
{
    if (store == NULL) {
        return;
    }
    for (size_t i = 0; i < TGC_SEM_MAX_SETS; ++i) {
        free(store->sets[i].values);
        free(store->sets[i].last_pids);
        free(store->sets[i].negative_waiters);
        free(store->sets[i].zero_waiters);
    }
    free(store);
}

int tgc_sem_store_get(struct tgc_sem_store *store, int32_t key, int nsems,
                      int flags)
{
    const struct tgc_sem_identity root_identity = {.uid = 0, .gid = 0};
    return tgc_sem_store_get_as(store, key, nsems, flags, root_identity);
}

int tgc_sem_store_get_as(struct tgc_sem_store *store, int32_t key, int nsems,
                         int flags, struct tgc_sem_identity identity)
{
    if (store == NULL) {
        return -EINVAL;
    }

    if (key != TGC_IPC_PRIVATE) {
        for (size_t i = 0; i < TGC_SEM_MAX_SETS; ++i) {
            struct tgc_sem_set *set = &store->sets[i];
            if (!set->active || set->key != key) {
                continue;
            }
            if ((flags & TGC_IPC_CREAT) != 0 &&
                (flags & TGC_IPC_EXCL) != 0) {
                return -EEXIST;
            }
            if (nsems < 0 || nsems > set->nsems) {
                return -EINVAL;
            }
            return make_id(i, set->generation);
        }
        if ((flags & TGC_IPC_CREAT) == 0) {
            return -ENOENT;
        }
    }

    if (nsems <= 0 || nsems > TGC_SEM_MAX_PER_SET) {
        return -EINVAL;
    }

    for (size_t i = 0; i < TGC_SEM_MAX_SETS; ++i) {
        struct tgc_sem_set *set = &store->sets[i];
        if (set->active) {
            continue;
        }

        uint16_t *values = calloc((size_t)nsems, sizeof(*values));
        int32_t *last_pids = calloc((size_t)nsems, sizeof(*last_pids));
        uint32_t *negative_waiters =
            calloc((size_t)nsems, sizeof(*negative_waiters));
        uint32_t *zero_waiters = calloc((size_t)nsems, sizeof(*zero_waiters));
        if (values == NULL || last_pids == NULL || negative_waiters == NULL ||
            zero_waiters == NULL) {
            free(values);
            free(last_pids);
            free(negative_waiters);
            free(zero_waiters);
            return -ENOMEM;
        }

        if (set->generation == 0) {
            set->generation = 1;
        }
        set->key = key;
        set->nsems = (uint16_t)nsems;
        set->uid = identity.uid;
        set->gid = identity.gid;
        set->cuid = identity.uid;
        set->cgid = identity.gid;
        set->mode = (uint32_t)flags & 0777U;
        set->otime = 0;
        set->ctime = current_time_seconds();
        set->values = values;
        set->last_pids = last_pids;
        set->negative_waiters = negative_waiters;
        set->zero_waiters = zero_waiters;
        set->active = true;
        return make_id(i, set->generation);
    }

    return -ENOSPC;
}

int tgc_sem_store_remove(struct tgc_sem_store *store, int semid)
{
    struct tgc_sem_set *set = find_set(store, semid);
    if (set == NULL) {
        return -EINVAL;
    }

    free(set->values);
    free(set->last_pids);
    free(set->negative_waiters);
    free(set->zero_waiters);
    set->values = NULL;
    set->last_pids = NULL;
    set->negative_waiters = NULL;
    set->zero_waiters = NULL;
    set->key = 0;
    set->nsems = 0;
    set->uid = 0;
    set->gid = 0;
    set->cuid = 0;
    set->cgid = 0;
    set->mode = 0;
    set->otime = 0;
    set->ctime = 0;
    set->active = false;
    set->generation = next_generation(set->generation);
    return 0;
}

int tgc_sem_store_getval(const struct tgc_sem_store *store, int semid,
                         size_t semnum)
{
    const struct tgc_sem_set *set = find_const_set(store, semid);
    if (set == NULL || semnum >= set->nsems) {
        return -EINVAL;
    }
    return set->values[semnum];
}

int tgc_sem_store_setval(struct tgc_sem_store *store, int semid,
                         size_t semnum, unsigned int value, int32_t pid)
{
    struct tgc_sem_set *set = find_set(store, semid);
    if (set == NULL || semnum >= set->nsems) {
        return -EINVAL;
    }
    if (value > TGC_SEM_MAX_VALUE) {
        return -ERANGE;
    }
    set->values[semnum] = (uint16_t)value;
    set->last_pids[semnum] = pid;
    set->ctime = current_time_seconds();
    return 0;
}

int tgc_sem_store_getpid(const struct tgc_sem_store *store, int semid,
                         size_t semnum)
{
    const struct tgc_sem_set *set = find_const_set(store, semid);
    if (set == NULL || semnum >= set->nsems) {
        return -EINVAL;
    }
    return set->last_pids[semnum];
}

int tgc_sem_store_getall(const struct tgc_sem_store *store, int semid,
                         uint16_t *values, size_t count)
{
    const struct tgc_sem_set *set = find_const_set(store, semid);
    if (set == NULL || values == NULL || count != set->nsems) {
        return -EINVAL;
    }
    memcpy(values, set->values, count * sizeof(*values));
    return 0;
}

int tgc_sem_store_setall(struct tgc_sem_store *store, int semid,
                         const uint16_t *values, size_t count, int32_t pid)
{
    struct tgc_sem_set *set = find_set(store, semid);
    if (set == NULL || values == NULL || count != set->nsems) {
        return -EINVAL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (values[i] > TGC_SEM_MAX_VALUE) {
            return -ERANGE;
        }
    }
    memcpy(set->values, values, count * sizeof(*values));
    for (size_t i = 0; i < count; ++i) {
        set->last_pids[i] = pid;
    }
    set->ctime = current_time_seconds();
    return 0;
}

int tgc_sem_store_get_metadata(const struct tgc_sem_store *store, int semid,
                               struct tgc_sem_metadata *metadata)
{
    const struct tgc_sem_set *set = find_const_set(store, semid);
    size_t index = 0;
    uint16_t generation = 0;
    if (set == NULL || metadata == NULL ||
        !decode_id(semid, &index, &generation)) {
        return -EINVAL;
    }
    *metadata = (struct tgc_sem_metadata){
        .key = set->key,
        .uid = set->uid,
        .gid = set->gid,
        .cuid = set->cuid,
        .cgid = set->cgid,
        .mode = set->mode,
        .nsems = set->nsems,
        .otime = set->otime,
        .ctime = set->ctime,
        .sequence = generation,
    };
    return 0;
}

int tgc_sem_store_set_metadata(struct tgc_sem_store *store, int semid,
                               uint32_t uid, uint32_t gid, uint32_t mode,
                               struct tgc_sem_identity actor)
{
    struct tgc_sem_set *set = find_set(store, semid);
    if (set == NULL) {
        return -EINVAL;
    }
    if (actor.uid != set->uid && actor.uid != set->cuid) {
        return -EPERM;
    }
    if (uid != set->uid || (gid != set->gid && gid != actor.gid) ||
        (mode & ~0777U) != 0U) {
        return -EPERM;
    }
    set->gid = gid;
    set->mode = mode;
    set->ctime = current_time_seconds();
    return 0;
}

int tgc_sem_store_get_wait_count(const struct tgc_sem_store *store, int semid,
                                 size_t semnum, int wait_for_zero)
{
    const struct tgc_sem_set *set = find_const_set(store, semid);
    if (set == NULL || semnum >= set->nsems) {
        return -EINVAL;
    }
    uint32_t count = wait_for_zero != 0 ? set->zero_waiters[semnum]
                                        : set->negative_waiters[semnum];
    return count > INT_MAX ? INT_MAX : (int)count;
}

int tgc_sem_store_adjust_wait_count(struct tgc_sem_store *store, int semid,
                                    size_t semnum, int wait_for_zero,
                                    int adjustment)
{
    struct tgc_sem_set *set = find_set(store, semid);
    if (set == NULL || semnum >= set->nsems ||
        (adjustment != 1 && adjustment != -1)) {
        return -EINVAL;
    }
    uint32_t *count = wait_for_zero != 0 ? &set->zero_waiters[semnum]
                                         : &set->negative_waiters[semnum];
    if ((adjustment > 0 && *count == UINT32_MAX) ||
        (adjustment < 0 && *count == 0)) {
        return -ERANGE;
    }
    *count = adjustment > 0 ? *count + 1 : *count - 1;
    return 0;
}

int tgc_sem_store_info(const struct tgc_sem_store *store, int dynamic,
                       struct tgc_sem_info *info)
{
    if (store == NULL || info == NULL) {
        return -EINVAL;
    }
    int highest_index = 0;
    int active_sets = 0;
    int total_semaphores = 0;
    for (size_t i = 0; i < TGC_SEM_MAX_SETS; ++i) {
        if (!store->sets[i].active) {
            continue;
        }
        active_sets += 1;
        total_semaphores += store->sets[i].nsems;
        highest_index = (int)i;
    }
    *info = (struct tgc_sem_info){
        .semmap = TGC_SEM_MAX_SETS,
        .semmni = TGC_SEM_MAX_SETS,
        .semmns = TGC_SEM_MAX_SETS * TGC_SEM_MAX_PER_SET,
        .semmnu = TGC_SEM_MAX_SETS,
        .semmsl = TGC_SEM_MAX_PER_SET,
        .semopm = TGC_SEM_MAX_PER_SET,
        .semume = TGC_SEM_MAX_PER_SET,
        .semusz = dynamic != 0 ? active_sets : (int32_t)sizeof(struct tgc_sem_set),
        .semvmx = TGC_SEM_MAX_VALUE,
        .semaem = dynamic != 0 ? total_semaphores : TGC_SEM_MAX_VALUE,
    };
    return highest_index;
}

int tgc_sem_store_tryop(struct tgc_sem_store *store, int semid,
                        const struct tgc_sem_op *operations, size_t count,
                        int32_t pid)
{
    return tgc_sem_store_tryop_detail(store, semid, operations, count, pid,
                                      NULL);
}

int tgc_sem_store_tryop_detail(struct tgc_sem_store *store, int semid,
                               const struct tgc_sem_op *operations,
                               size_t count, int32_t pid,
                               struct tgc_sem_wait_reason *wait_reason)
{
    struct tgc_sem_set *set = find_set(store, semid);
    if (set == NULL || operations == NULL || count == 0) {
        return -EINVAL;
    }

    const unsigned int allowed_flags = TGC_IPC_NOWAIT | TGC_SEM_UNDO;
    for (size_t i = 0; i < count; ++i) {
        if (operations[i].sem_num >= set->nsems ||
            (operations[i].sem_flg & ~allowed_flags) != 0) {
            return -EINVAL;
        }
        if ((operations[i].sem_flg & TGC_SEM_UNDO) != 0) {
            return -ENOTSUP;
        }
    }

    uint16_t working[TGC_SEM_MAX_PER_SET];
    memcpy(working, set->values, set->nsems * sizeof(*working));

    for (size_t i = 0; i < count; ++i) {
        const struct tgc_sem_op *operation = &operations[i];
        uint16_t *value = &working[operation->sem_num];
        bool blocked = false;

        if (operation->sem_op > 0) {
            unsigned int result = (unsigned int)*value +
                                  (unsigned int)operation->sem_op;
            if (result > TGC_SEM_MAX_VALUE) {
                return -ERANGE;
            }
            *value = (uint16_t)result;
        } else if (operation->sem_op < 0) {
            unsigned int amount = (unsigned int)-(int)operation->sem_op;
            if (*value < amount) {
                blocked = true;
            } else {
                *value = (uint16_t)(*value - amount);
            }
        } else if (*value != 0) {
            blocked = true;
        }

        if (blocked) {
            if (wait_reason != NULL) {
                wait_reason->sem_num = operation->sem_num;
                wait_reason->wait_for_zero = operation->sem_op == 0 ? 1U : 0U;
            }
            return (operation->sem_flg & TGC_IPC_NOWAIT) != 0
                       ? -EAGAIN
                       : TGC_SEM_OP_BLOCKED;
        }
    }

    memcpy(set->values, working, set->nsems * sizeof(*working));
    for (size_t i = 0; i < count; ++i) {
        set->last_pids[operations[i].sem_num] = pid;
    }
    set->otime = current_time_seconds();
    return 0;
}
