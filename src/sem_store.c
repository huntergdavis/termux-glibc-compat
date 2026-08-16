#include <tgcompat/sem_store.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
    uint16_t *values;
    int32_t *last_pids;
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
    }
    free(store);
}

int tgc_sem_store_get(struct tgc_sem_store *store, int32_t key, int nsems,
                      int flags)
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
        if (values == NULL || last_pids == NULL) {
            free(values);
            free(last_pids);
            return -ENOMEM;
        }

        if (set->generation == 0) {
            set->generation = 1;
        }
        set->key = key;
        set->nsems = (uint16_t)nsems;
        set->values = values;
        set->last_pids = last_pids;
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
    set->values = NULL;
    set->last_pids = NULL;
    set->key = 0;
    set->nsems = 0;
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
    return 0;
}
