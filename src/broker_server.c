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

static int wait_for_state_change(struct tgc_broker *broker)
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
    int result = pthread_cond_timedwait(&broker->state_changed, &broker->mutex,
                                        &deadline);
    return result == 0 || result == ETIMEDOUT ? 0 : -result;
}

static int dispatch_request(struct tgc_broker *broker, int socket_fd,
                            const struct tgc_protocol_packet *request,
                            int32_t peer_pid,
                            struct tgc_protocol_packet *response)
{
    int result = pthread_mutex_lock(&broker->mutex);
    if (result != 0) {
        return -result;
    }

    result = tgc_broker_dispatch(broker->store, request, peer_pid, response);
    if (result != 0) {
        (void)pthread_mutex_unlock(&broker->mutex);
        return result;
    }
    if (request->header.opcode != TGC_OPCODE_SEMOP ||
        response->header.result != TGC_SEM_OP_BLOCKED) {
        if (operation_changed_state(request, response)) {
            (void)pthread_cond_broadcast(&broker->state_changed);
        }
        (void)pthread_mutex_unlock(&broker->mutex);
        return 0;
    }

    struct tgc_waiter waiter = {
        .semid = tgc_wire_get_i32(request->payload),
        .next = NULL,
    };
    enqueue_waiter(broker, &waiter);
    for (;;) {
        if (peer_has_closed(socket_fd)) {
            remove_waiter(broker, &waiter);
            (void)pthread_cond_broadcast(&broker->state_changed);
            (void)pthread_mutex_unlock(&broker->mutex);
            return 0;
        }
        if (waiter_is_first_for_set(broker, &waiter)) {
            result = tgc_broker_dispatch(broker->store, request, peer_pid,
                                         response);
            if (result != 0 ||
                response->header.result != TGC_SEM_OP_BLOCKED) {
                remove_waiter(broker, &waiter);
                (void)pthread_cond_broadcast(&broker->state_changed);
                (void)pthread_mutex_unlock(&broker->mutex);
                return result;
            }
        }
        result = wait_for_state_change(broker);
        if (result != 0) {
            remove_waiter(broker, &waiter);
            (void)pthread_cond_broadcast(&broker->state_changed);
            (void)pthread_mutex_unlock(&broker->mutex);
            return result;
        }
    }
}

int tgc_broker_serve_connection(struct tgc_broker *broker, int socket_fd,
                                uid_t expected_uid)
{
    if (broker == NULL || socket_fd < 0) {
        return -EINVAL;
    }

    pid_t peer_pid = 0;
    int result = tgc_transport_authenticate(socket_fd, expected_uid, &peer_pid);
    if (result != 0) {
        return result;
    }
    if (peer_pid > INT32_MAX) {
        return -EOVERFLOW;
    }

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
                                  (int32_t)peer_pid, &response);
        if (result != 0) {
            return result;
        }
        result = tgc_transport_send(socket_fd, &response);
        if (result != 0) {
            return result;
        }
    }
}
