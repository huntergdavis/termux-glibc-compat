#include <tgcompat/broker.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(void)
{
    int failed = 0;
    struct tgc_sem_store *store = tgc_sem_store_create();
    struct tgc_protocol_packet request;
    struct tgc_protocol_packet response;
    CHECK(store != NULL);

    request_init(&request, TGC_OPCODE_PING, 1, 0);
    CHECK(tgc_broker_dispatch(store, &request, &response) == 0);
    CHECK(response.header.kind == TGC_PROTOCOL_RESPONSE);
    CHECK(response.header.request_id == 1 && response.header.result == 0);

    request_init(&request, TGC_OPCODE_SEMGET, 2, 12);
    tgc_wire_put_i32(request.payload, 1234);
    tgc_wire_put_i32(request.payload + 4, 2);
    tgc_wire_put_u32(request.payload + 8, TGC_IPC_CREAT | 0600U);
    CHECK(tgc_broker_dispatch(store, &request, &response) == 0);
    int semid = response.header.result;
    CHECK(semid > 0);

    request_init(&request, TGC_OPCODE_SETALL, 3, 16);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, 2);
    tgc_wire_put_i32(request.payload + 8, 500);
    tgc_wire_put_u16(request.payload + 12, 3);
    tgc_wire_put_u16(request.payload + 14, 4);
    CHECK(tgc_broker_dispatch(store, &request, &response) == 0);
    CHECK(response.header.result == 0);

    request_init(&request, TGC_OPCODE_GETALL, 4, 8);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, 2);
    CHECK(tgc_broker_dispatch(store, &request, &response) == 0);
    CHECK(response.header.result == 0 && response.header.payload_length == 8);
    CHECK(tgc_wire_get_u32(response.payload) == 2);
    CHECK(tgc_wire_get_u16(response.payload + 4) == 3);
    CHECK(tgc_wire_get_u16(response.payload + 6) == 4);

    request_init(&request, TGC_OPCODE_SEMOP, 5, 20);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, 1);
    tgc_wire_put_i32(request.payload + 8, 600);
    tgc_wire_put_u16(request.payload + 12, 0);
    tgc_wire_put_i16(request.payload + 14, -2);
    tgc_wire_put_u16(request.payload + 16, 0);
    tgc_wire_put_u16(request.payload + 18, 0);
    CHECK(tgc_broker_dispatch(store, &request, &response) == 0);
    CHECK(response.header.result == 0);

    request_init(&request, TGC_OPCODE_GETVAL, 6, 8);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, 0);
    CHECK(tgc_broker_dispatch(store, &request, &response) == 0);
    CHECK(response.header.result == 1);

    request.header.payload_length = 7;
    CHECK(tgc_broker_dispatch(store, &request, &response) == 0);
    CHECK(response.header.result == -EMSGSIZE);
    CHECK(response.header.payload_length == 0);

    request_init(&request, TGC_OPCODE_REMOVE, 7, 4);
    tgc_wire_put_i32(request.payload, semid);
    CHECK(tgc_broker_dispatch(store, &request, &response) == 0);
    CHECK(response.header.result == 0);

done:
    tgc_sem_store_destroy(store);
    if (!failed) {
        puts("broker-dispatch: PASS");
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
