#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { THREAD_COUNT = 4, ITERATIONS = 25000 };

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t counter;

static void *worker(void *unused)
{
    (void)unused;
    for (int i = 0; i < ITERATIONS; ++i) {
        if (pthread_mutex_lock(&lock) != 0) {
            return (void *)(uintptr_t)1;
        }
        ++counter;
        if (pthread_mutex_unlock(&lock) != 0) {
            return (void *)(uintptr_t)1;
        }
    }
    return NULL;
}

int main(void)
{
    pthread_t threads[THREAD_COUNT];

    for (size_t i = 0; i < THREAD_COUNT; ++i) {
        int rc = pthread_create(&threads[i], NULL, worker, NULL);
        if (rc != 0) {
            fprintf(stderr, "pthread_create[%zu] failed: %d\n", i, rc);
            return EXIT_FAILURE;
        }
    }

    for (size_t i = 0; i < THREAD_COUNT; ++i) {
        void *result = NULL;
        int rc = pthread_join(threads[i], &result);
        if (rc != 0 || result != NULL) {
            fprintf(stderr, "pthread_join[%zu] failed: rc=%d worker=%p\n",
                    i, rc, result);
            return EXIT_FAILURE;
        }
    }

    uint64_t expected = (uint64_t)THREAD_COUNT * ITERATIONS;
    printf("pthread counter=%llu expected=%llu\n",
           (unsigned long long)counter, (unsigned long long)expected);
    return counter == expected ? EXIT_SUCCESS : EXIT_FAILURE;
}
