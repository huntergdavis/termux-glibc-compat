#define _GNU_SOURCE

#include <tgcompat/broker.h>
#include <tgcompat/transport.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <unistd.h>

enum {
    MAX_CLIENTS = 64,
};

struct client_worker {
    struct tgc_broker *broker;
    pthread_t thread;
    uid_t expected_uid;
    int socket_fd;
    int result;
    atomic_bool done;
    int active;
};

static int create_signal_fd(void)
{
    sigset_t mask;
    if (sigemptyset(&mask) != 0 || sigaddset(&mask, SIGINT) != 0 ||
        sigaddset(&mask, SIGTERM) != 0) {
        return -errno;
    }
    int result = pthread_sigmask(SIG_BLOCK, &mask, NULL);
    if (result != 0) {
        return -result;
    }
    int signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    return signal_fd >= 0 ? signal_fd : -errno;
}

static void usage(FILE *stream, const char *program)
{
    fprintf(stream, "Usage: %s [--once] --socket ABSOLUTE_PATH\n", program);
}

static void *serve_client(void *argument)
{
    struct client_worker *worker = argument;
    worker->result = tgc_broker_serve_connection(
        worker->broker, worker->socket_fd, worker->expected_uid);
    (void)close(worker->socket_fd);
    atomic_store_explicit(&worker->done, true, memory_order_release);
    return NULL;
}

static void report_worker_result(const struct client_worker *worker)
{
    if (worker->result != 0) {
        fprintf(stderr, "tgcompatd: client: %s\n", strerror(-worker->result));
    }
}

static void reap_finished_workers(struct client_worker workers[MAX_CLIENTS])
{
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (workers[i].active == 0 ||
            !atomic_load_explicit(&workers[i].done, memory_order_acquire)) {
            continue;
        }
        (void)pthread_join(workers[i].thread, NULL);
        report_worker_result(&workers[i]);
        workers[i].active = 0;
    }
}

static struct client_worker *available_worker(
    struct client_worker workers[MAX_CLIENTS])
{
    reap_finished_workers(workers);
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (workers[i].active == 0) {
            return &workers[i];
        }
    }
    return NULL;
}

static void stop_and_join_workers(struct client_worker workers[MAX_CLIENTS])
{
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (workers[i].active != 0) {
            (void)shutdown(workers[i].socket_fd, SHUT_RDWR);
        }
    }
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (workers[i].active == 0) {
            continue;
        }
        (void)pthread_join(workers[i].thread, NULL);
        report_worker_result(&workers[i]);
        workers[i].active = 0;
    }
}

int main(int argc, char **argv)
{
    const char *socket_path = getenv("TGCOMPAT_SOCKET");
    int once = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--once") == 0) {
            once = 1;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        } else {
            usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (socket_path == NULL || socket_path[0] == '\0') {
        usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    int signal_fd = create_signal_fd();
    if (signal_fd < 0) {
        fprintf(stderr, "tgcompatd: signal setup: %s\n", strerror(-signal_fd));
        return EXIT_FAILURE;
    }

    uid_t uid = geteuid();
    int listener_fd = tgc_transport_listen(socket_path, uid);
    if (listener_fd < 0) {
        fprintf(stderr, "tgcompatd: listen %s: %s\n", socket_path,
                strerror(-listener_fd));
        (void)close(signal_fd);
        return EXIT_FAILURE;
    }
    struct tgc_broker *broker = tgc_broker_create();
    if (broker == NULL) {
        fprintf(stderr, "tgcompatd: state allocation: %s\n", strerror(errno));
        (void)close(listener_fd);
        (void)close(signal_fd);
        (void)unlink(socket_path);
        return EXIT_FAILURE;
    }
    pthread_attr_t worker_attributes;
    int result = pthread_attr_init(&worker_attributes);
    int worker_attributes_initialized = result == 0;
    if (result == 0) {
        result = pthread_attr_setstacksize(&worker_attributes, 256U * 1024U);
    }
    if (result != 0) {
        fprintf(stderr, "tgcompatd: worker attributes: %s\n", strerror(result));
        if (worker_attributes_initialized != 0) {
            (void)pthread_attr_destroy(&worker_attributes);
        }
        tgc_broker_destroy(broker);
        (void)close(listener_fd);
        (void)close(signal_fd);
        (void)unlink(socket_path);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "tgcompatd: listening on %s\n", socket_path);
    int failed = 0;
    struct client_worker workers[MAX_CLIENTS];
    memset(workers, 0, sizeof(workers));
    struct pollfd poll_fds[2] = {
        {.fd = signal_fd, .events = POLLIN},
        {.fd = listener_fd, .events = POLLIN},
    };
    for (;;) {
        int poll_result = poll(poll_fds, 2, -1);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "tgcompatd: poll: %s\n", strerror(errno));
            failed = 1;
            break;
        }
        if ((poll_fds[0].revents & POLLIN) != 0) {
            struct signalfd_siginfo signal_info;
            ssize_t bytes = read(signal_fd, &signal_info,
                                 sizeof(signal_info));
            if (bytes != (ssize_t)sizeof(signal_info) &&
                !(bytes < 0 && errno == EAGAIN)) {
                fprintf(stderr, "tgcompatd: signal read: %s\n",
                        bytes < 0 ? strerror(errno) : "short read");
                failed = 1;
            }
            break;
        }
        if ((poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (poll_fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            fprintf(stderr, "tgcompatd: poll descriptor failure\n");
            failed = 1;
            break;
        }
        if ((poll_fds[1].revents & POLLIN) == 0) {
            continue;
        }
        int client_fd = accept4(listener_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "tgcompatd: accept: %s\n", strerror(errno));
            failed = 1;
            break;
        }
        if (once != 0) {
            result = tgc_broker_serve_connection(broker, client_fd, uid);
            (void)close(client_fd);
            if (result != 0) {
                fprintf(stderr, "tgcompatd: client: %s\n", strerror(-result));
                failed = 1;
            }
            break;
        }

        struct client_worker *worker = available_worker(workers);
        if (worker == NULL) {
            fprintf(stderr, "tgcompatd: client limit reached\n");
            (void)close(client_fd);
            continue;
        }
        worker->broker = broker;
        worker->expected_uid = uid;
        worker->socket_fd = client_fd;
        worker->result = 0;
        worker->active = 1;
        atomic_store_explicit(&worker->done, false, memory_order_relaxed);
        result = pthread_create(&worker->thread, &worker_attributes,
                                serve_client, worker);
        if (result != 0) {
            fprintf(stderr, "tgcompatd: pthread_create: %s\n", strerror(result));
            worker->active = 0;
            (void)close(client_fd);
        }
    }

    (void)pthread_attr_destroy(&worker_attributes);
    (void)close(listener_fd);
    (void)close(signal_fd);
    stop_and_join_workers(workers);
    tgc_broker_destroy(broker);
    if (unlink(socket_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "tgcompatd: unlink %s: %s\n", socket_path,
                strerror(errno));
        failed = 1;
    }
    return failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
