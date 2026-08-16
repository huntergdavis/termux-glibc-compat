/* SPDX-License-Identifier: LGPL-2.1-or-later */

#ifndef TGCOMPAT_GLIBC_H
#define TGCOMPAT_GLIBC_H

#include <tgcompat/sem_store.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/sem.h>

int __tgcompat_glibc_semget(key_t key, int nsems, int semflg);
int __tgcompat_glibc_remove(int semid);
int __tgcompat_glibc_getval(int semid, int semnum);
int __tgcompat_glibc_setval(int semid, int semnum, int value);
int __tgcompat_glibc_getpid(int semid, int semnum);
int __tgcompat_glibc_getncnt(int semid, int semnum);
int __tgcompat_glibc_getzcnt(int semid, int semnum);
int __tgcompat_glibc_getall(int semid, unsigned short int *values);
int __tgcompat_glibc_setall(int semid, const unsigned short int *values);
int __tgcompat_glibc_stat(int semid, struct tgc_sem_metadata *metadata);
int __tgcompat_glibc_stat_index(int index,
                                struct tgc_sem_metadata *metadata);
int __tgcompat_glibc_set_metadata(int semid, uint32_t uid, uint32_t gid,
                                  uint32_t mode);
int __tgcompat_glibc_info(int dynamic, struct tgc_sem_info *info);
int __tgcompat_glibc_semtimedop(int semid, struct sembuf *operations,
                                size_t count, int has_timeout,
                                int64_t timeout_seconds,
                                int64_t timeout_nanoseconds);

#endif
