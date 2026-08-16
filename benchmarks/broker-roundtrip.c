#define _GNU_SOURCE

#include <tgcompat/client.h>
#include <tgcompat/protocol.h>
#include <tgcompat/sem_store.h>
#include <tgcompat/transport.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../tests/temp-path.h"

static void request_init(struct tgc_protocol_packet *request, uint16_t opcode,
                         uint32_t payload_length)
{
    memset(request, 0, sizeof(*request));
    request->header.version = TGC_PROTOCOL_VERSION;
    request->header.kind = TGC_PROTOCOL_REQUEST;
    request->header.opcode = opcode;
    request->header.payload_length = payload_length;
}

static int connect_when_ready(const char *socket_path)
{
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    int written = snprintf(address.sun_path, sizeof(address.sun_path), "%s",
                           socket_path);
    if (written < 0 || (size_t)written >= sizeof(address.sun_path)) {
        return -ENAMETOOLONG;
    }

    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000L};
    for (int attempt = 0; attempt < 200; ++attempt) {
        int socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (socket_fd < 0) {
            return -errno;
        }
        if (connect(socket_fd, (const struct sockaddr *)&address,
                    sizeof(address)) == 0) {
            return socket_fd;
        }
        int saved_errno = errno;
        (void)close(socket_fd);
        if (saved_errno != ENOENT && saved_errno != ECONNREFUSED) {
            return -saved_errno;
        }
        (void)nanosleep(&pause, NULL);
    }
    return -ETIMEDOUT;
}

static int exchange(int socket_fd, struct tgc_protocol_packet *request,
                    int expected_result)
{
    static uint32_t request_id;
    struct tgc_protocol_packet response;
    request->header.request_id = ++request_id;
    int result = tgc_transport_send(socket_fd, request);
    if (result != 0) {
        return result;
    }
    result = tgc_transport_receive(socket_fd, &response);
    if (result != 0) {
        return result;
    }
    if (response.header.request_id != request->header.request_id ||
        response.header.result != expected_result ||
        response.header.payload_length != 0) {
        return -EPROTO;
    }
    return 0;
}

static uint64_t elapsed_nanoseconds(const struct timespec *start,
                                    const struct timespec *end)
{
    uint64_t seconds = (uint64_t)(end->tv_sec - start->tv_sec);
    int64_t nanoseconds = end->tv_nsec - start->tv_nsec;
    if (nanoseconds < 0) {
        seconds -= 1;
        nanoseconds += 1000000000L;
    }
    return (seconds * 1000000000ULL) + (uint64_t)nanoseconds;
}

static int run_case(const char *name, int socket_fd,
                    struct tgc_protocol_packet *request, int expected_result,
                    uint32_t iterations)
{
    for (uint32_t i = 0; i < 1000; ++i) {
        int result = exchange(socket_fd, request, expected_result);
        if (result != 0) {
            return result;
        }
    }

    struct timespec start;
    struct timespec end;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return -errno;
    }
    for (uint32_t i = 0; i < iterations; ++i) {
        int result = exchange(socket_fd, request, expected_result);
        if (result != 0) {
            return result;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        return -errno;
    }

    uint64_t elapsed = elapsed_nanoseconds(&start, &end);
    double nanoseconds_per_operation = (double)elapsed / (double)iterations;
    double operations_per_second = 1000000000.0 / nanoseconds_per_operation;
    printf("%s iterations=%u elapsed_ms=%.3f ns_per_op=%.1f ops_per_second=%.0f\n",
           name, iterations, (double)elapsed / 1000000.0,
           nanoseconds_per_operation, operations_per_second);
    return 0;
}

typedef int (*client_case_fn)(struct tgc_client *client, int semid);

static int client_ping_case(struct tgc_client *client, int semid)
{
    (void)semid;
    return tgc_client_ping(client);
}

static int client_getval_case(struct tgc_client *client, int semid)
{
    int result = tgc_client_getval(client, semid, 0);
    return result == 7 ? 0 : result < 0 ? result : -EPROTO;
}

static int run_client_case(const char *name, struct tgc_client *client,
                           int semid, client_case_fn operation,
                           uint32_t iterations)
{
    for (uint32_t i = 0; i < 1000; ++i) {
        int result = operation(client, semid);
        if (result != 0) {
            return result;
        }
    }

    struct timespec start;
    struct timespec end;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return -errno;
    }
    for (uint32_t i = 0; i < iterations; ++i) {
        int result = operation(client, semid);
        if (result != 0) {
            return result;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        return -errno;
    }

    uint64_t elapsed = elapsed_nanoseconds(&start, &end);
    double nanoseconds_per_operation = (double)elapsed / (double)iterations;
    printf("%s iterations=%u elapsed_ms=%.3f ns_per_op=%.1f ops_per_second=%.0f\n",
           name, iterations, (double)elapsed / 1000000.0,
           nanoseconds_per_operation, 1000000000.0 / nanoseconds_per_operation);
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t iterations = 100000;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [ITERATIONS]\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 2) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || parsed == 0 ||
            parsed > UINT32_MAX) {
            fprintf(stderr, "invalid iteration count: %s\n", argv[1]);
            return EXIT_FAILURE;
        }
        iterations = (uint32_t)parsed;
    }

    int failed = 0;
    char directory[TGC_TEST_PATH_CAPACITY];
    char *created_directory = NULL;
    char socket_path[sizeof(directory) + 24];
    pid_t daemon_pid = -1;
    int socket_fd = -1;
    struct tgc_client client;
    int client_initialized = 0;
    if (tgc_test_temp_template(directory, sizeof(directory),
                               "tgcompat-benchmark") == 0) {
        created_directory = mkdtemp(directory);
    }
    if (created_directory == NULL ||
        snprintf(socket_path, sizeof(socket_path), "%s/broker.sock",
                 created_directory) <= 0) {
        perror("benchmark setup");
        return EXIT_FAILURE;
    }

    daemon_pid = fork();
    if (daemon_pid < 0) {
        perror("fork");
        failed = 1;
        goto done;
    }
    if (daemon_pid == 0) {
        execl("./build/tgcompatd", "tgcompatd", "--socket", socket_path,
              (char *)NULL);
        _exit(127);
    }
    socket_fd = connect_when_ready(socket_path);
    if (socket_fd < 0) {
        fprintf(stderr, "connect: %s\n", strerror(-socket_fd));
        failed = 1;
        goto done;
    }

    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_SEMGET, 12);
    tgc_wire_put_i32(request.payload, TGC_IPC_PRIVATE);
    tgc_wire_put_i32(request.payload + 4, 1);
    tgc_wire_put_u32(request.payload + 8, TGC_IPC_CREAT | 0600U);
    struct tgc_protocol_packet response;
    request.header.request_id = 1;
    if (tgc_transport_send(socket_fd, &request) != 0 ||
        tgc_transport_receive(socket_fd, &response) != 0 ||
        response.header.result <= 0) {
        fprintf(stderr, "semget setup failed\n");
        failed = 1;
        goto done;
    }
    int semid = response.header.result;

    request_init(&request, TGC_OPCODE_SETVAL, 12);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, 0);
    tgc_wire_put_u32(request.payload + 8, 7);
    if (exchange(socket_fd, &request, 0) != 0) {
        fprintf(stderr, "setval setup failed\n");
        failed = 1;
        goto done;
    }

    request_init(&request, TGC_OPCODE_PING, 0);
    int result = run_case("ping", socket_fd, &request, 0, iterations);
    if (result != 0) {
        fprintf(stderr, "ping benchmark: %s\n", strerror(-result));
        failed = 1;
        goto done;
    }

    request_init(&request, TGC_OPCODE_GETVAL, 8);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, 0);
    result = run_case("getval", socket_fd, &request, 7, iterations);
    if (result != 0) {
        fprintf(stderr, "getval benchmark: %s\n", strerror(-result));
        failed = 1;
        goto done;
    }

    result = tgc_client_init(&client, socket_path);
    if (result != 0) {
        fprintf(stderr, "client init: %s\n", strerror(-result));
        failed = 1;
        goto done;
    }
    client_initialized = 1;
    result = run_client_case("client-ping", &client, semid,
                             client_ping_case, iterations);
    if (result == 0) {
        result = run_client_case("client-getval", &client, semid,
                                 client_getval_case, iterations);
    }
    if (result != 0) {
        fprintf(stderr, "client benchmark: %s\n", strerror(-result));
        failed = 1;
    }

done:
    if (client_initialized != 0) {
        tgc_client_close(&client);
    }
    if (socket_fd >= 0) {
        (void)close(socket_fd);
    }
    if (daemon_pid > 0) {
        int status = 0;
        (void)kill(daemon_pid, SIGTERM);
        if (waitpid(daemon_pid, &status, 0) != daemon_pid ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            failed = 1;
        }
    }
    if (created_directory != NULL) {
        (void)unlink(socket_path);
        if (rmdir(created_directory) != 0) {
            failed = 1;
        }
    }
    return failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
