#include <tgcompat/sem_store.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            failed = 1;                                                         \
            goto done;                                                          \
        }                                                                       \
    } while (0)

int main(void)
{
    int failed = 0;
    struct tgc_sem_store *store = tgc_sem_store_create();
    CHECK(store != NULL);

    CHECK(tgc_sem_store_get(NULL, 1234, 2, TGC_IPC_CREAT) == -EINVAL);
    CHECK(tgc_sem_store_get(store, 1234, 2, 0) == -ENOENT);
    int keyed = tgc_sem_store_get(store, 1234, 2, TGC_IPC_CREAT);
    CHECK(keyed > 0);
    CHECK(tgc_sem_store_get(store, 1234, 0, 0) == keyed);
    CHECK(tgc_sem_store_get(store, 1234, -1, 0) == -EINVAL);
    CHECK(tgc_sem_store_get(store, 1234, 3, 0) == -EINVAL);
    CHECK(tgc_sem_store_get(store, 1234, 2,
                            TGC_IPC_CREAT | TGC_IPC_EXCL) == -EEXIST);

    uint16_t values[2] = {3, 4};
    CHECK(tgc_sem_store_setall(store, keyed, values, 2, 101) == 0);
    CHECK(tgc_sem_store_getval(store, keyed, 0) == 3);
    CHECK(tgc_sem_store_getval(store, keyed, 1) == 4);
    CHECK(tgc_sem_store_getpid(store, keyed, 0) == 101);
    CHECK(tgc_sem_store_getpid(store, keyed, 1) == 101);
    CHECK(tgc_sem_store_getval(store, keyed, 2) == -EINVAL);

    uint16_t observed[2] = {0, 0};
    CHECK(tgc_sem_store_getall(store, keyed, observed, 2) == 0);
    CHECK(observed[0] == 3 && observed[1] == 4);
    CHECK(tgc_sem_store_getall(store, keyed, observed, 1) == -EINVAL);

    uint16_t invalid[2] = {7, TGC_SEM_MAX_VALUE + 1};
    CHECK(tgc_sem_store_setall(store, keyed, invalid, 2, 202) == -ERANGE);
    CHECK(tgc_sem_store_getval(store, keyed, 0) == 3);
    CHECK(tgc_sem_store_getval(store, keyed, 1) == 4);
    CHECK(tgc_sem_store_getpid(store, keyed, 0) == 101);

    CHECK(tgc_sem_store_setval(store, keyed, 0, 9, 303) == 0);
    CHECK(tgc_sem_store_getval(store, keyed, 0) == 9);
    CHECK(tgc_sem_store_getpid(store, keyed, 0) == 303);
    CHECK(tgc_sem_store_setval(store, keyed, 0, TGC_SEM_MAX_VALUE + 1,
                               404) == -ERANGE);
    CHECK(tgc_sem_store_getval(store, keyed, 0) == 9);

    CHECK(tgc_sem_store_remove(store, keyed) == 0);
    CHECK(tgc_sem_store_getval(store, keyed, 0) == -EINVAL);
    int replacement = tgc_sem_store_get(store, TGC_IPC_PRIVATE, 1, 0);
    CHECK(replacement > 0);
    CHECK(replacement != keyed);
    CHECK((replacement & 0xffff) == (keyed & 0xffff));

    int second_private = tgc_sem_store_get(store, TGC_IPC_PRIVATE, 1, 0);
    CHECK(second_private > 0 && second_private != replacement);
    CHECK(tgc_sem_store_remove(store, replacement) == 0);
    CHECK(tgc_sem_store_remove(store, second_private) == 0);

done:
    tgc_sem_store_destroy(store);
    if (!failed) {
        puts("sem-store: PASS");
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
