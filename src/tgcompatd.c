#define _GNU_SOURCE

#include <tgcompat/broker.h>
#include <tgcompat/transport.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        return -errno;
    }
    return 0;
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

    int result = install_signal_handlers();
    if (result != 0) {
        fprintf(stderr, "tgcompatd: signal setup: %s\n", strerror(-result));
        return EXIT_FAILURE;
    }

    uid_t uid = geteuid();
    int listener_fd = tgc_transport_listen(socket_path, uid);
    if (listener_fd < 0) {
        fprintf(stderr, "tgcompatd: listen %s: %s\n", socket_path,
                strerror(-listener_fd));
        return EXIT_FAILURE;
    }
    struct tgc_broker *broker = tgc_broker_create();
    if (broker == NULL) {
        fprintf(stderr, "tgcompatd: state allocation: %s\n", strerror(errno));
        (void)close(listener_fd);
        (void)unlink(socket_path);
        return EXIT_FAILURE;
    }
    pthread_attr_t worker_attributes;
    result = pthread_attr_init(&worker_attributes);
    int worker_attributes_initialized = result == 0;
    if (result == 0) {
        result = pthread_attr_setstacksize(&worker_attributes, 256U * 1024U);
    }
    sigset_t worker_signal_mask;
    if (result == 0) {
        result = sigemptyset(&worker_signal_mask);
    }
    if (result == 0) {
        result = sigaddset(&worker_signal_mask, SIGINT);
    }
    if (result == 0) {
        result = sigaddset(&worker_signal_mask, SIGTERM);
    }
    if (result == 0) {
        result = pthread_attr_setsigmask_np(&worker_attributes,
                                            &worker_signal_mask);
    }
    if (result != 0) {
        fprintf(stderr, "tgcompatd: worker attributes: %s\n", strerror(result));
        if (worker_attributes_initialized != 0) {
            (void)pthread_attr_destroy(&worker_attributes);
        }
        tgc_broker_destroy(broker);
        (void)close(listener_fd);
        (void)unlink(socket_path);
        return EXIT_FAILURE;
    }

    fprintf(stderr, "tgcompatd: listening on %s\n", socket_path);
    int failed = 0;
    struct client_worker workers[MAX_CLIENTS];
    memset(workers, 0, sizeof(workers));
    while (stop_requested == 0) {
        int client_fd = accept4(listener_fd, NULL, NULL, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EINTR && stop_requested != 0) {
                break;
            }
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
    stop_and_join_workers(workers);
    tgc_broker_destroy(broker);
    if (unlink(socket_path) != 0 && errno != ENOENT) {
        fprintf(stderr, "tgcompatd: unlink %s: %s\n", socket_path,
                strerror(errno));
        failed = 1;
    }
    return failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
