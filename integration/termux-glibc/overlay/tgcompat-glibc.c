/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <tgcompat/client.h>
#include <tgcompat/glibc.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef __set_errno
#define __set_errno(value) (errno = (value))
#endif

static __thread struct tgc_client thread_client;
static __thread int thread_client_initialized;
static pthread_key_t client_cleanup_key;
static pthread_once_t client_cleanup_once = PTHREAD_ONCE_INIT;
static int client_cleanup_error;

static void close_thread_client(void *value)
{
    struct tgc_client *client = value;
    if (client != NULL) {
        tgc_client_close(client);
    }
    thread_client_initialized = 0;
}

static void create_client_cleanup_key(void)
{
    client_cleanup_error =
        pthread_key_create(&client_cleanup_key, close_thread_client);
}

static struct tgc_client *client_for_thread(void)
{
    if (thread_client_initialized == 0) {
        const char *socket_path = secure_getenv("TGCOMPAT_SOCKET");
        if (socket_path == NULL || socket_path[0] == '\0') {
            __set_errno(ENOSYS);
            return NULL;
        }
        int result = pthread_once(&client_cleanup_once,
                                  create_client_cleanup_key);
        if (result == 0) {
            result = client_cleanup_error;
        }
        if (result != 0) {
            __set_errno(result);
            return NULL;
        }
        result = tgc_client_init(&thread_client, socket_path);
        if (result != 0) {
            __set_errno(-result);
            return NULL;
        }
        result = pthread_setspecific(client_cleanup_key, &thread_client);
        if (result != 0) {
            tgc_client_close(&thread_client);
            __set_errno(result);
            return NULL;
        }
        thread_client_initialized = 1;
    }
    return &thread_client;
}

static int libc_result(int result)
{
    if (result < 0) {
        __set_errno(-result);
        return -1;
    }
    return result;
}

int __tgcompat_glibc_semget(key_t key, int nsems, int semflg)
{
    struct tgc_client *client = client_for_thread();
    return client == NULL
               ? -1
               : libc_result(tgc_client_semget(client, (int32_t)key, nsems,
                                                semflg));
}

int __tgcompat_glibc_remove(int semid)
{
    struct tgc_client *client = client_for_thread();
    return client == NULL ? -1 : libc_result(tgc_client_remove(client, semid));
}

static int semnum_call(int semid, int semnum,
                       int (*operation)(struct tgc_client *, int, size_t))
{
    if (semnum < 0) {
        __set_errno(EINVAL);
        return -1;
    }
    struct tgc_client *client = client_for_thread();
    return client == NULL
               ? -1
               : libc_result(operation(client, semid, (size_t)semnum));
}

int __tgcompat_glibc_getval(int semid, int semnum)
{
    return semnum_call(semid, semnum, tgc_client_getval);
}

int __tgcompat_glibc_setval(int semid, int semnum, int value)
{
    if (semnum < 0 || value < 0) {
        __set_errno(EINVAL);
        return -1;
    }
    struct tgc_client *client = client_for_thread();
    return client == NULL
               ? -1
               : libc_result(tgc_client_setval(client, semid, (size_t)semnum,
                                                (unsigned int)value));
}

int __tgcompat_glibc_getpid(int semid, int semnum)
{
    return semnum_call(semid, semnum, tgc_client_getpid);
}

int __tgcompat_glibc_getncnt(int semid, int semnum)
{
    return semnum_call(semid, semnum, tgc_client_getncnt);
}

int __tgcompat_glibc_getzcnt(int semid, int semnum)
{
    return semnum_call(semid, semnum, tgc_client_getzcnt);
}

static int semaphore_count(struct tgc_client *client, int semid,
                           size_t *count)
{
    struct tgc_sem_metadata metadata;
    int result = tgc_client_stat(client, semid, &metadata);
    if (result != 0) {
        return result;
    }
    if (metadata.nsems == 0 || metadata.nsems > TGC_SEM_MAX_PER_SET) {
        return -EPROTO;
    }
    *count = metadata.nsems;
    return 0;
}

int __tgcompat_glibc_getall(int semid, unsigned short int *values)
{
    if (values == NULL) {
        __set_errno(EFAULT);
        return -1;
    }
    struct tgc_client *client = client_for_thread();
    if (client == NULL) {
        return -1;
    }
    size_t count = 0;
    int result = semaphore_count(client, semid, &count);
    return result != 0
               ? libc_result(result)
               : libc_result(tgc_client_getall(client, semid, values, count));
}

int __tgcompat_glibc_setall(int semid, const unsigned short int *values)
{
    if (values == NULL) {
        __set_errno(EFAULT);
        return -1;
    }
    struct tgc_client *client = client_for_thread();
    if (client == NULL) {
        return -1;
    }
    size_t count = 0;
    int result = semaphore_count(client, semid, &count);
    return result != 0
               ? libc_result(result)
               : libc_result(tgc_client_setall(client, semid, values, count));
}

int __tgcompat_glibc_stat(int semid, struct tgc_sem_metadata *metadata)
{
    if (metadata == NULL) {
        __set_errno(EFAULT);
        return -1;
    }
    struct tgc_client *client = client_for_thread();
    return client == NULL
               ? -1
               : libc_result(tgc_client_stat(client, semid, metadata));
}

int __tgcompat_glibc_stat_index(int index,
                                struct tgc_sem_metadata *metadata)
{
    if (index < 0 || metadata == NULL) {
        __set_errno(index < 0 ? EINVAL : EFAULT);
        return -1;
    }
    struct tgc_client *client = client_for_thread();
    return client == NULL
               ? -1
               : libc_result(tgc_client_stat_index(client, (size_t)index,
                                                   metadata));
}

int __tgcompat_glibc_set_metadata(int semid, uint32_t uid, uint32_t gid,
                                  uint32_t mode)
{
    struct tgc_client *client = client_for_thread();
    return client == NULL
               ? -1
               : libc_result(tgc_client_set_metadata(client, semid, uid, gid,
                                                      mode));
}

int __tgcompat_glibc_info(int dynamic, struct tgc_sem_info *info)
{
    if (info == NULL) {
        __set_errno(EFAULT);
        return -1;
    }
    struct tgc_client *client = client_for_thread();
    return client == NULL
               ? -1
               : libc_result(tgc_client_info(client, dynamic, info));
}

int __tgcompat_glibc_semtimedop(int semid, struct sembuf *operations,
                                size_t count, int has_timeout,
                                int64_t timeout_seconds,
                                int64_t timeout_nanoseconds)
{
    if (operations == NULL || count == 0 || count > TGC_SEM_MAX_PER_SET ||
        (has_timeout != 0 &&
         (timeout_seconds < 0 || timeout_nanoseconds < 0 ||
          timeout_nanoseconds >= 1000000000LL ||
          timeout_seconds >
              (INT64_MAX - timeout_nanoseconds) / 1000000000LL))) {
        __set_errno(EINVAL);
        return -1;
    }
    struct tgc_sem_op converted[TGC_SEM_MAX_PER_SET];
    for (size_t i = 0; i < count; ++i) {
        converted[i] = (struct tgc_sem_op){
            .sem_num = operations[i].sem_num,
            .sem_op = operations[i].sem_op,
            .sem_flg = (uint16_t)operations[i].sem_flg,
        };
    }
    struct tgc_client *client = client_for_thread();
    if (client == NULL) {
        return -1;
    }
    int result = has_timeout != 0
                     ? tgc_client_semtimedop(
                           client, semid, converted, count,
                           timeout_seconds * 1000000000LL +
                               timeout_nanoseconds)
                     : tgc_client_semop(client, semid, converted, count);
    return libc_result(result);
}
