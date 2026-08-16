#define _GNU_SOURCE

#include <tgcompat/protocol.h>
#include <tgcompat/sem_store.h>
#include <tgcompat/transport.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
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

int main(void)
{
    int failed = 0;
    int sockets[2] = {-1, -1};
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);

    pid_t peer_pid = 0;
    CHECK(tgc_transport_authenticate(sockets[0], geteuid(), &peer_pid) == 0);
    CHECK(peer_pid == getpid());
    CHECK(tgc_transport_authenticate(sockets[0], geteuid() + 1, &peer_pid) ==
          -EACCES);

    struct tgc_protocol_packet sent;
    memset(&sent, 0, sizeof(sent));
    sent.header.version = TGC_PROTOCOL_VERSION;
    sent.header.kind = TGC_PROTOCOL_REQUEST;
    sent.header.opcode = TGC_OPCODE_SEMGET;
    sent.header.request_id = 42;
    sent.header.payload_length = 12;
    tgc_wire_put_i32(sent.payload, 1234);
    tgc_wire_put_i32(sent.payload + 4, 2);
    tgc_wire_put_u32(sent.payload + 8, TGC_IPC_CREAT | 0600U);
    CHECK(tgc_transport_send(sockets[0], &sent) == 0);

    struct tgc_protocol_packet received;
    CHECK(tgc_transport_receive(sockets[1], &received) == 0);
    CHECK(received.header.request_id == 42);
    CHECK(received.header.payload_length == 12);
    CHECK(memcmp(received.payload, sent.payload, 12) == 0);

    int closed_socket = sockets[0];
    sockets[0] = -1;
    CHECK(close(closed_socket) == 0);
    CHECK(tgc_transport_receive(sockets[1], &received) == TGC_TRANSPORT_EOF);

done:
    if (sockets[0] >= 0) {
        (void)close(sockets[0]);
    }
    if (sockets[1] >= 0) {
        (void)close(sockets[1]);
    }
    if (!failed) {
        puts("transport: PASS");
    }
    return failed != 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
