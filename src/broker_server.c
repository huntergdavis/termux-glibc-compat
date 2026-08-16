#define _GNU_SOURCE

#include <tgcompat/broker.h>
#include <tgcompat/transport.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>

struct tgc_waiter {
    int semid;
    uint16_t semnum;
    uint16_t wait_for_zero;
    int counted;
    int has_deadline;
    struct timespec deadline;
    struct tgc_waiter *next;
};

struct tgc_broker {
    struct tgc_sem_store *store;
    pthread_mutex_t mutex;
    pthread_cond_t state_changed;
    struct tgc_waiter *waiters_head;
    struct tgc_waiter *waiters_tail;
};

struct tgc_broker *tgc_broker_create(void)
{
    struct tgc_broker *broker = calloc(1, sizeof(*broker));
    if (broker == NULL) {
        return NULL;
    }
    broker->store = tgc_sem_store_create();
    if (broker->store == NULL) {
        free(broker);
        return NULL;
    }
    int result = pthread_mutex_init(&broker->mutex, NULL);
    if (result != 0) {
        tgc_sem_store_destroy(broker->store);
        free(broker);
        errno = result;
        return NULL;
    }

    pthread_condattr_t attributes;
    int attributes_initialized = 0;
    result = pthread_condattr_init(&attributes);
    if (result == 0) {
        attributes_initialized = 1;
        result = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    }
    if (result == 0) {
        result = pthread_cond_init(&broker->state_changed, &attributes);
    }
    if (attributes_initialized != 0) {
        (void)pthread_condattr_destroy(&attributes);
    }
    if (result != 0) {
        (void)pthread_mutex_destroy(&broker->mutex);
        tgc_sem_store_destroy(broker->store);
        free(broker);
        errno = result;
        return NULL;
    }
    return broker;
}

void tgc_broker_destroy(struct tgc_broker *broker)
{
    if (broker == NULL) {
        return;
    }
    (void)pthread_cond_destroy(&broker->state_changed);
    (void)pthread_mutex_destroy(&broker->mutex);
    tgc_sem_store_destroy(broker->store);
    free(broker);
}

static int operation_changed_state(
    const struct tgc_protocol_packet *request,
    const struct tgc_protocol_packet *response)
{
    if (response->header.result != 0) {
        return 0;
    }
    return request->header.opcode == TGC_OPCODE_REMOVE ||
           request->header.opcode == TGC_OPCODE_SETVAL ||
           request->header.opcode == TGC_OPCODE_SETALL ||
           request->header.opcode == TGC_OPCODE_SEMOP;
}

static void enqueue_waiter(struct tgc_broker *broker,
                           struct tgc_waiter *waiter)
{
    waiter->next = NULL;
    if (broker->waiters_tail == NULL) {
        broker->waiters_head = waiter;
    } else {
        broker->waiters_tail->next = waiter;
    }
    broker->waiters_tail = waiter;
}

static void remove_waiter(struct tgc_broker *broker,
                          const struct tgc_waiter *waiter)
{
    struct tgc_waiter *previous = NULL;
    struct tgc_waiter *current = broker->waiters_head;
    while (current != NULL && current != waiter) {
        previous = current;
        current = current->next;
    }
    if (current == NULL) {
        return;
    }
    if (previous == NULL) {
        broker->waiters_head = current->next;
    } else {
        previous->next = current->next;
    }
    if (broker->waiters_tail == current) {
        broker->waiters_tail = previous;
    }
}

static int waiter_is_first_for_set(const struct tgc_broker *broker,
                                   const struct tgc_waiter *waiter)
{
    for (const struct tgc_waiter *current = broker->waiters_head;
         current != NULL; current = current->next) {
        if (current == waiter) {
            return 1;
        }
        if (current->semid == waiter->semid) {
            return 0;
        }
    }
    return 0;
}

static int peer_has_closed(int socket_fd)
{
    char byte;
    ssize_t result = recv(socket_fd, &byte, sizeof(byte),
                          MSG_PEEK | MSG_DONTWAIT);
    if (result == 0) {
        return 1;
    }
    return result < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
           errno != EINTR;
}

static int compare_timespec(const struct timespec *left,
                            const struct timespec *right)
{
    if (left->tv_sec != right->tv_sec) {
        return left->tv_sec < right->tv_sec ? -1 : 1;
    }
    return left->tv_nsec == right->tv_nsec
               ? 0
               : (left->tv_nsec < right->tv_nsec ? -1 : 1);
}

static int initialize_deadline(const struct tgc_protocol_packet *request,
                               struct tgc_waiter *waiter)
{
    if (request->header.opcode != TGC_OPCODE_SEMTIMEDOP) {
        return 0;
    }
    int64_t timeout_ns = tgc_wire_get_i64(request->payload + 8);
    if (timeout_ns < 0 || clock_gettime(CLOCK_MONOTONIC, &waiter->deadline) !=
                              0) {
        return timeout_ns < 0 ? -EINVAL : -errno;
    }
    int64_t seconds = timeout_ns / 1000000000LL;
    long nanoseconds = (long)(timeout_ns % 1000000000LL);
    if (seconds > INT_MAX || waiter->deadline.tv_sec > INT_MAX - seconds) {
        waiter->deadline.tv_sec = INT_MAX;
        waiter->deadline.tv_nsec = 999999999L;
    } else {
        waiter->deadline.tv_sec += (time_t)seconds;
        waiter->deadline.tv_nsec += nanoseconds;
        if (waiter->deadline.tv_nsec >= 1000000000L) {
            waiter->deadline.tv_sec += 1;
            waiter->deadline.tv_nsec -= 1000000000L;
        }
    }
    waiter->has_deadline = 1;
    return 0;
}

static int wait_for_state_change(struct tgc_broker *broker,
                                 const struct tgc_waiter *waiter,
                                 int *operation_timed_out)
{
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        return -errno;
    }
    deadline.tv_nsec += 250000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    if (waiter->has_deadline != 0 &&
        compare_timespec(&waiter->deadline, &deadline) < 0) {
        deadline = waiter->deadline;
    }
    int result = pthread_cond_timedwait(&broker->state_changed, &broker->mutex,
                                        &deadline);
    if (result != 0 && result != ETIMEDOUT) {
        return -result;
    }
    *operation_timed_out = 0;
    if (waiter->has_deadline != 0) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return -errno;
        }
        *operation_timed_out =
            compare_timespec(&now, &waiter->deadline) >= 0;
    }
    return 0;
}

static void finish_waiter(struct tgc_broker *broker,
                          struct tgc_waiter *waiter)
{
    remove_waiter(broker, waiter);
    if (waiter->counted != 0) {
        (void)tgc_sem_store_adjust_wait_count(
            broker->store, waiter->semid, waiter->semnum,
            waiter->wait_for_zero, -1);
        waiter->counted = 0;
    }
}

static int dispatch_request(struct tgc_broker *broker, int socket_fd,
                            const struct tgc_protocol_packet *request,
                            struct tgc_broker_actor actor,
                            struct tgc_protocol_packet *response)
{
    int result = pthread_mutex_lock(&broker->mutex);
    if (result != 0) {
        return -result;
    }

    result = tgc_broker_dispatch(broker->store, request, actor, response);
    if (result != 0) {
        (void)pthread_mutex_unlock(&broker->mutex);
        return result;
    }
    if ((request->header.opcode != TGC_OPCODE_SEMOP &&
         request->header.opcode != TGC_OPCODE_SEMTIMEDOP) ||
        response->header.result != TGC_SEM_OP_BLOCKED) {
        if (operation_changed_state(request, response)) {
            (void)pthread_cond_broadcast(&broker->state_changed);
        }
        (void)pthread_mutex_unlock(&broker->mutex);
        return 0;
    }

    struct tgc_waiter waiter = {
        .semid = tgc_wire_get_i32(request->payload),
        .semnum = response->header.payload_length == 8
                      ? tgc_wire_get_u16(response->payload)
                      : UINT16_MAX,
        .wait_for_zero = response->header.payload_length == 8
                             ? tgc_wire_get_u16(response->payload + 2)
                             : UINT16_MAX,
        .counted = 0,
        .has_deadline = 0,
        .next = NULL,
    };
    if (response->header.payload_length != 8 || waiter.wait_for_zero > 1 ||
        tgc_wire_get_u32(response->payload + 4) != 0) {
        (void)pthread_mutex_unlock(&broker->mutex);
        return -EPROTO;
    }
    result = initialize_deadline(request, &waiter);
    if (result != 0) {
        (void)pthread_mutex_unlock(&broker->mutex);
        return result;
    }
    result = tgc_sem_store_adjust_wait_count(
        broker->store, waiter.semid, waiter.semnum, waiter.wait_for_zero, 1);
    if (result != 0) {
        (void)pthread_mutex_unlock(&broker->mutex);
        return result;
    }
    waiter.counted = 1;
    enqueue_waiter(broker, &waiter);
    for (;;) {
        if (peer_has_closed(socket_fd)) {
            finish_waiter(broker, &waiter);
            (void)pthread_cond_broadcast(&broker->state_changed);
            (void)pthread_mutex_unlock(&broker->mutex);
            return 0;
        }
        if (waiter_is_first_for_set(broker, &waiter)) {
            result = tgc_broker_dispatch(broker->store, request, actor,
                                         response);
            if (result != 0 ||
                response->header.result != TGC_SEM_OP_BLOCKED) {
                finish_waiter(broker, &waiter);
                (void)pthread_cond_broadcast(&broker->state_changed);
                (void)pthread_mutex_unlock(&broker->mutex);
                return result;
            }
        }
        int operation_timed_out = 0;
        result = wait_for_state_change(broker, &waiter,
                                       &operation_timed_out);
        if (result != 0) {
            finish_waiter(broker, &waiter);
            (void)pthread_cond_broadcast(&broker->state_changed);
            (void)pthread_mutex_unlock(&broker->mutex);
            return result;
        }
        if (operation_timed_out != 0) {
            finish_waiter(broker, &waiter);
            response->header.result = -EAGAIN;
            response->header.payload_length = 0;
            (void)pthread_cond_broadcast(&broker->state_changed);
            (void)pthread_mutex_unlock(&broker->mutex);
            return 0;
        }
    }
}

int tgc_broker_serve_connection(struct tgc_broker *broker, int socket_fd,
                                uid_t expected_uid)
{
    if (broker == NULL || socket_fd < 0) {
        return -EINVAL;
    }

    struct tgc_peer_credentials credentials;
    int result = tgc_transport_get_credentials(socket_fd, expected_uid,
                                               &credentials);
    if (result != 0) {
        return result;
    }
    if (credentials.pid > INT32_MAX || credentials.uid > UINT32_MAX ||
        credentials.gid > UINT32_MAX) {
        return -EOVERFLOW;
    }
    const struct tgc_broker_actor actor = {
        .pid = (int32_t)credentials.pid,
        .identity = {
            .uid = (uint32_t)credentials.uid,
            .gid = (uint32_t)credentials.gid,
        },
    };

    for (;;) {
        struct tgc_protocol_packet request;
        result = tgc_transport_receive(socket_fd, &request);
        if (result == TGC_TRANSPORT_EOF) {
            return 0;
        }
        if (result != 0) {
            return result;
        }
        if (request.header.kind != TGC_PROTOCOL_REQUEST) {
            return -EPROTO;
        }

        struct tgc_protocol_packet response;
        result = dispatch_request(broker, socket_fd, &request,
                                  actor, &response);
        if (result != 0) {
            return result;
        }
        result = tgc_transport_send(socket_fd, &response);
        if (result != 0) {
            return result;
        }
    }
}
