#define _GNU_SOURCE

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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            failed = 1;                                                         \
            goto done;                                                          \
        }                                                                       \
    } while (0)

static void request_init(struct tgc_protocol_packet *request, uint16_t opcode,
                         uint32_t request_id, uint32_t payload_length)
{
    memset(request, 0, sizeof(*request));
    request->header.version = TGC_PROTOCOL_VERSION;
    request->header.kind = TGC_PROTOCOL_REQUEST;
    request->header.opcode = opcode;
    request->header.request_id = request_id;
    request->header.payload_length = payload_length;
}

static int connect_when_ready(const char *socket_path)
{
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);

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

int main(void)
{
    int failed = 0;
    char directory[] = "/tmp/tgcompat-integration.XXXXXX";
    char *created_directory = mkdtemp(directory);
    char socket_path[sizeof(directory) + 24];
    pid_t daemon_pid = -1;
    int socket_fd = -1;
    CHECK(created_directory != NULL);
    CHECK(snprintf(socket_path, sizeof(socket_path), "%s/broker.sock",
                   created_directory) > 0);

    daemon_pid = fork();
    CHECK(daemon_pid >= 0);
    if (daemon_pid == 0) {
        execl("./build/tgcompatd", "tgcompatd", "--once", "--socket",
              socket_path, (char *)NULL);
        _exit(127);
    }

    socket_fd = connect_when_ready(socket_path);
    CHECK(socket_fd >= 0);

    struct tgc_protocol_packet request;
    struct tgc_protocol_packet response;
    request_init(&request, TGC_OPCODE_PING, 1, 0);
    CHECK(tgc_transport_send(socket_fd, &request) == 0);
    CHECK(tgc_transport_receive(socket_fd, &response) == 0);
    CHECK(response.header.request_id == 1 && response.header.result == 0);

    request_init(&request, TGC_OPCODE_SEMGET, 2, 12);
    tgc_wire_put_i32(request.payload, 4321);
    tgc_wire_put_i32(request.payload + 4, 1);
    tgc_wire_put_u32(request.payload + 8, TGC_IPC_CREAT | 0600U);
    CHECK(tgc_transport_send(socket_fd, &request) == 0);
    CHECK(tgc_transport_receive(socket_fd, &response) == 0);
    int semid = response.header.result;
    CHECK(semid > 0);

    request_init(&request, TGC_OPCODE_SETVAL, 3, 12);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, 0);
    tgc_wire_put_u32(request.payload + 8, 9);
    CHECK(tgc_transport_send(socket_fd, &request) == 0);
    CHECK(tgc_transport_receive(socket_fd, &response) == 0);
    CHECK(response.header.result == 0);

    request_init(&request, TGC_OPCODE_GETPID, 4, 8);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, 0);
    CHECK(tgc_transport_send(socket_fd, &request) == 0);
    CHECK(tgc_transport_receive(socket_fd, &response) == 0);
    CHECK(response.header.result == getpid());

done:
    if (socket_fd >= 0) {
        (void)close(socket_fd);
    }
    if (daemon_pid > 0) {
        int status = 0;
        if (socket_fd < 0) {
            (void)kill(daemon_pid, SIGTERM);
        }
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
    if (!failed) {
        puts("broker-integration: PASS");
    }
    return failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
