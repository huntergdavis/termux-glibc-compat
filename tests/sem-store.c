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

    values[0] = 1;
    values[1] = 0;
    CHECK(tgc_sem_store_setall(store, keyed, values, 2, 505) == 0);
    struct tgc_sem_op blocked[2] = {
        {.sem_num = 0, .sem_op = -1, .sem_flg = 0},
        {.sem_num = 1, .sem_op = -1, .sem_flg = 0},
    };
    CHECK(tgc_sem_store_tryop(store, keyed, blocked, 2, 606) ==
          TGC_SEM_OP_BLOCKED);
    CHECK(tgc_sem_store_getval(store, keyed, 0) == 1);
    CHECK(tgc_sem_store_getval(store, keyed, 1) == 0);

    blocked[1].sem_flg = TGC_IPC_NOWAIT;
    CHECK(tgc_sem_store_tryop(store, keyed, blocked, 2, 606) == -EAGAIN);
    CHECK(tgc_sem_store_getval(store, keyed, 0) == 1);
    blocked[1].sem_flg = 0;

    CHECK(tgc_sem_store_setval(store, keyed, 1, 1, 707) == 0);
    CHECK(tgc_sem_store_tryop(store, keyed, blocked, 2, 808) == 0);
    CHECK(tgc_sem_store_getval(store, keyed, 0) == 0);
    CHECK(tgc_sem_store_getval(store, keyed, 1) == 0);
    CHECK(tgc_sem_store_getpid(store, keyed, 0) == 808);
    CHECK(tgc_sem_store_getpid(store, keyed, 1) == 808);

    struct tgc_sem_op wait_for_zero = {
        .sem_num = 0, .sem_op = 0, .sem_flg = 0,
    };
    CHECK(tgc_sem_store_tryop(store, keyed, &wait_for_zero, 1, 909) == 0);
    CHECK(tgc_sem_store_getpid(store, keyed, 0) == 909);

    struct tgc_sem_op overflow[2] = {
        {.sem_num = 0, .sem_op = 1, .sem_flg = 0},
        {.sem_num = 1, .sem_op = TGC_SEM_MAX_VALUE, .sem_flg = 0},
    };
    CHECK(tgc_sem_store_setval(store, keyed, 1, 1, 1001) == 0);
    CHECK(tgc_sem_store_tryop(store, keyed, overflow, 2, 1002) == -ERANGE);
    CHECK(tgc_sem_store_getval(store, keyed, 0) == 0);
    CHECK(tgc_sem_store_getval(store, keyed, 1) == 1);

    struct tgc_sem_op undo = {
        .sem_num = 0, .sem_op = 1, .sem_flg = TGC_SEM_UNDO,
    };
    CHECK(tgc_sem_store_tryop(store, keyed, &undo, 1, 1101) == -ENOTSUP);
    struct tgc_sem_op invalid_sem = {
        .sem_num = 2, .sem_op = 1, .sem_flg = 0,
    };
    CHECK(tgc_sem_store_tryop(store, keyed, &invalid_sem, 1, 1102) ==
          -EINVAL);
    CHECK(tgc_sem_store_tryop(store, keyed, NULL, 1, 1103) == -EINVAL);
    CHECK(tgc_sem_store_tryop(store, keyed, &undo, 0, 1104) == -EINVAL);

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
