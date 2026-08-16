#include <tgcompat/broker.h>
#include <tgcompat/transport.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>

int tgc_broker_serve_connection(struct tgc_sem_store *store, int socket_fd,
                                uid_t expected_uid)
{
    if (store == NULL || socket_fd < 0) {
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
        result = tgc_broker_dispatch(store, &request, (int32_t)peer_pid,
                                     &response);
        if (result != 0) {
            return result;
        }
        result = tgc_transport_send(socket_fd, &response);
        if (result != 0) {
            return result;
        }
    }
}
