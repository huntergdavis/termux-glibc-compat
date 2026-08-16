#define _GNU_SOURCE

#include <tgcompat/broker.h>
#include <tgcompat/transport.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum {
    TGC_MAX_UNDO_PROCESSES = 1024,
    PROCESS_CHECK_INTERVAL_NS = 250000000,
};

struct tgc_waiter {
    int semid;
    uint16_t semnum;
    uint16_t wait_for_zero;
    int counted;
    int has_deadline;
    struct timespec deadline;
    struct tgc_waiter *next;
};

struct tgc_process_watcher {
    int32_t pid;
    int pidfd;
    ino_t proc_inode;
    int active;
};

struct tgc_broker {
    struct tgc_sem_store *store;
    pthread_mutex_t mutex;
    pthread_cond_t state_changed;
    struct tgc_waiter *waiters_head;
    struct tgc_waiter *waiters_tail;
    struct tgc_process_watcher process_watchers[TGC_MAX_UNDO_PROCESSES];
    pthread_t process_monitor;
    int process_monitor_started;
    int stop_process_monitor;
};

static int open_pidfd(int32_t pid)
{
#if defined(SYS_pidfd_open)
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
#elif defined(__NR_pidfd_open)
    int fd = (int)syscall(__NR_pidfd_open, pid, 0);
#else
    int fd = -1;
    errno = ENOSYS;
#endif
    return fd >= 0 ? fd : -errno;
}

static int proc_path(char output[64], int32_t pid, const char *suffix)
{
    int length = snprintf(output, 64, "/proc/%ld%s", (long)pid, suffix);
    return length > 0 && length < 64 ? 0 : -EOVERFLOW;
}

static int read_proc_identity(int32_t pid, ino_t *inode)
{
    char path[64];
    int result = proc_path(path, pid, "");
    if (result != 0) {
        return result;
    }
    struct stat status;
    if (stat(path, &status) != 0) {
        return -errno;
    }
    *inode = status.st_ino;
    return 0;
}

static int proc_state_is_exited(int32_t pid, ino_t expected_inode)
{
    ino_t current_inode = 0;
    int result = read_proc_identity(pid, &current_inode);
    if (result == -ENOENT || (result == 0 && current_inode != expected_inode)) {
        return 1;
    }
    if (result != 0) {
        return 0;
    }

    char path[64];
    if (proc_path(path, pid, "/stat") != 0) {
        return 0;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? 1 : 0;
    }
    char buffer[256];
    ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    (void)close(fd);
    if (count <= 0) {
        return 0;
    }
    buffer[count] = '\0';
    char *right_parenthesis = strrchr(buffer, ')');
    return right_parenthesis != NULL && right_parenthesis[1] == ' ' &&
           (right_parenthesis[2] == 'Z' || right_parenthesis[2] == 'X');
}

static int process_has_exited(const struct tgc_process_watcher *watcher)
{
    if (watcher->pidfd >= 0) {
        struct pollfd descriptor = {
            .fd = watcher->pidfd,
            .events = POLLIN,
            .revents = 0,
        };
        int result = poll(&descriptor, 1, 0);
        return result > 0 &&
               (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    }
    return proc_state_is_exited(watcher->pid, watcher->proc_inode);
}

static int has_process_watchers(const struct tgc_broker *broker)
{
    for (size_t i = 0; i < TGC_MAX_UNDO_PROCESSES; ++i) {
        if (broker->process_watchers[i].active != 0) {
            return 1;
        }
    }
    return 0;
}

static void *monitor_process_exits(void *argument)
{
    struct tgc_broker *broker = argument;
    int result = pthread_mutex_lock(&broker->mutex);
    if (result != 0) {
        return NULL;
    }
    while (broker->stop_process_monitor == 0) {
        for (size_t i = 0; i < TGC_MAX_UNDO_PROCESSES; ++i) {
            struct tgc_process_watcher *watcher =
                &broker->process_watchers[i];
            if (watcher->active == 0 || !process_has_exited(watcher)) {
                continue;
            }
            int changed =
                tgc_sem_store_process_exit(broker->store, watcher->pid);
            if (watcher->pidfd >= 0) {
                (void)close(watcher->pidfd);
            }
            *watcher = (struct tgc_process_watcher){.pidfd = -1};
            if (changed > 0) {
                (void)pthread_cond_broadcast(&broker->state_changed);
            }
        }

        if (broker->stop_process_monitor != 0) {
            break;
        }
        if (!has_process_watchers(broker)) {
            (void)pthread_cond_wait(&broker->state_changed, &broker->mutex);
            continue;
        }
        struct timespec deadline;
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
            break;
        }
        deadline.tv_nsec += PROCESS_CHECK_INTERVAL_NS;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        result = pthread_cond_timedwait(&broker->state_changed,
                                        &broker->mutex, &deadline);
        if (result != 0 && result != ETIMEDOUT) {
            break;
        }
    }
    (void)pthread_mutex_unlock(&broker->mutex);
    return NULL;
}

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
    for (size_t i = 0; i < TGC_MAX_UNDO_PROCESSES; ++i) {
        broker->process_watchers[i].pidfd = -1;
    }

    pthread_attr_t monitor_attributes;
    result = pthread_attr_init(&monitor_attributes);
    int monitor_attributes_initialized = result == 0;
    if (result == 0) {
        result = pthread_attr_setstacksize(&monitor_attributes,
                                           128U * 1024U);
    }
    if (result == 0) {
        result = pthread_create(&broker->process_monitor,
                                &monitor_attributes,
                                monitor_process_exits, broker);
    }
    if (monitor_attributes_initialized != 0) {
        (void)pthread_attr_destroy(&monitor_attributes);
    }
    if (result != 0) {
        (void)pthread_cond_destroy(&broker->state_changed);
        (void)pthread_mutex_destroy(&broker->mutex);
        tgc_sem_store_destroy(broker->store);
        free(broker);
        errno = result;
        return NULL;
    }
    broker->process_monitor_started = 1;
    return broker;
}

void tgc_broker_destroy(struct tgc_broker *broker)
{
    if (broker == NULL) {
        return;
    }
    if (broker->process_monitor_started != 0) {
        if (pthread_mutex_lock(&broker->mutex) == 0) {
            broker->stop_process_monitor = 1;
            (void)pthread_cond_broadcast(&broker->state_changed);
            (void)pthread_mutex_unlock(&broker->mutex);
        }
        (void)pthread_join(broker->process_monitor, NULL);
    }
    for (size_t i = 0; i < TGC_MAX_UNDO_PROCESSES; ++i) {
        if (broker->process_watchers[i].active != 0 &&
            broker->process_watchers[i].pidfd >= 0) {
            (void)close(broker->process_watchers[i].pidfd);
        }
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
           request->header.opcode == TGC_OPCODE_SEMOP ||
           request->header.opcode == TGC_OPCODE_SEMTIMEDOP;
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

static int request_uses_undo(const struct tgc_protocol_packet *request)
{
    if (request->header.opcode != TGC_OPCODE_SEMOP &&
        request->header.opcode != TGC_OPCODE_SEMTIMEDOP) {
        return 0;
    }
    uint32_t prefix_size = request->header.opcode == TGC_OPCODE_SEMTIMEDOP
                               ? 16U
                               : 8U;
    if (request->header.payload_length < prefix_size) {
        return 0;
    }
    uint32_t count = tgc_wire_get_u32(request->payload + 4);
    if (count == 0 || count > TGC_SEM_MAX_PER_SET ||
        request->header.payload_length != prefix_size + (count * 8U)) {
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *operation = request->payload + prefix_size + (i * 8U);
        if ((tgc_wire_get_u16(operation + 4) & TGC_SEM_UNDO) != 0) {
            return 1;
        }
    }
    return 0;
}

static int watch_process(struct tgc_broker *broker, int32_t pid)
{
    struct tgc_process_watcher *available = NULL;
    for (size_t i = 0; i < TGC_MAX_UNDO_PROCESSES; ++i) {
        struct tgc_process_watcher *watcher = &broker->process_watchers[i];
        if (watcher->active != 0 && watcher->pid == pid) {
            return 0;
        }
        if (watcher->active == 0 && available == NULL) {
            available = watcher;
        }
    }
    if (available == NULL) {
        return -ENOSPC;
    }

    int pidfd = open_pidfd(pid);
    ino_t proc_inode = 0;
    if (pidfd < 0) {
        int result = read_proc_identity(pid, &proc_inode);
        if (result != 0) {
            return result;
        }
    }
    *available = (struct tgc_process_watcher){
        .pid = pid,
        .pidfd = pidfd >= 0 ? pidfd : -1,
        .proc_inode = proc_inode,
        .active = 1,
    };
    (void)pthread_cond_broadcast(&broker->state_changed);
    return 0;
}

static void prepare_error_response(
    const struct tgc_protocol_packet *request,
    struct tgc_protocol_packet *response, int result)
{
    response->header = (struct tgc_protocol_header){
        .version = TGC_PROTOCOL_VERSION,
        .kind = TGC_PROTOCOL_RESPONSE,
        .opcode = request->header.opcode,
        .request_id = request->header.request_id,
        .payload_length = 0,
        .result = result,
    };
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

    if (request_uses_undo(request)) {
        result = watch_process(broker, actor.pid);
        if (result != 0) {
            prepare_error_response(request, response, result);
            (void)pthread_mutex_unlock(&broker->mutex);
            return 0;
        }
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

    struct tgc_peer_credentials credentials = {0};
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
