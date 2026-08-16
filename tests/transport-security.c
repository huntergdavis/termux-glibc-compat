#define _GNU_SOURCE

#include <tgcompat/protocol.h>
#include <tgcompat/transport.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "temp-path.h"

#define CHECK(expression)                                                       \
    do {                                                                        \
        if (!(expression)) {                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #expression);                                               \
            failed = 1;                                                         \
            goto done;                                                          \
        }                                                                       \
    } while (0)

int main(void)
{
    int failed = 0;
    char directory[TGC_TEST_PATH_CAPACITY];
    char *created_directory = NULL;
    char socket_path[sizeof(directory) + 24];
    int listener_fd = -1;
    int sockets[2] = {-1, -1};
    CHECK(tgc_test_temp_template(directory, sizeof(directory),
                                 "tgcompat-security") == 0);
    created_directory = mkdtemp(directory);
    CHECK(created_directory != NULL);
    CHECK(snprintf(socket_path, sizeof(socket_path), "%s/broker.sock",
                   created_directory) > 0);

    listener_fd = tgc_transport_listen(socket_path, geteuid());
    CHECK(listener_fd >= 0);
    struct stat socket_status;
    CHECK(lstat(socket_path, &socket_status) == 0);
    CHECK(S_ISSOCK(socket_status.st_mode));
    CHECK(socket_status.st_uid == geteuid());
    CHECK((socket_status.st_mode & 0777U) == 0600U);
    CHECK(tgc_transport_listen(socket_path, geteuid()) == -EADDRINUSE);
    int closed_listener = listener_fd;
    listener_fd = -1;
    CHECK(close(closed_listener) == 0);
    CHECK(unlink(socket_path) == 0);

    CHECK(chmod(created_directory, 0755) == 0);
    CHECK(tgc_transport_listen(socket_path, geteuid()) == -EPERM);
    CHECK(chmod(created_directory, 0700) == 0);

    int ordinary_fd = open(socket_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                           0600);
    CHECK(ordinary_fd >= 0);
    CHECK(close(ordinary_fd) == 0);
    CHECK(tgc_transport_listen(socket_path, geteuid()) == -EADDRINUSE);
    CHECK(lstat(socket_path, &socket_status) == 0);
    CHECK(S_ISREG(socket_status.st_mode));
    CHECK(unlink(socket_path) == 0);
    CHECK(tgc_transport_listen("relative/broker.sock", geteuid()) == -EINVAL);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    struct tgc_protocol_header header = {
        .version = TGC_PROTOCOL_VERSION,
        .kind = TGC_PROTOCOL_REQUEST,
        .opcode = TGC_OPCODE_REMOVE,
        .request_id = 9,
        .payload_length = 4,
        .result = 0,
    };
    uint8_t wire_header[TGC_PROTOCOL_HEADER_SIZE];
    CHECK(tgc_protocol_encode_header(wire_header, &header) == 0);
    CHECK(send(sockets[0], wire_header, sizeof(wire_header), MSG_NOSIGNAL) ==
          (ssize_t)sizeof(wire_header));
    CHECK(shutdown(sockets[0], SHUT_WR) == 0);
    struct tgc_protocol_packet packet;
    CHECK(tgc_transport_receive(sockets[1], &packet) == -EPROTO);

done:
    if (listener_fd >= 0) {
        (void)close(listener_fd);
    }
    for (size_t i = 0; i < 2; ++i) {
        if (sockets[i] >= 0) {
            (void)close(sockets[i]);
        }
    }
    if (created_directory != NULL) {
        (void)unlink(socket_path);
        if (rmdir(created_directory) != 0) {
            failed = 1;
        }
    }
    if (!failed) {
        puts("transport-security: PASS");
    }
    return failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
