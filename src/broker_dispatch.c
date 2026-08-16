#include <tgcompat/broker.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

enum {
    SEMGET_SIZE = 12,
    SEMID_SIZE = 4,
    SEMNUM_SIZE = 8,
    SETVAL_SIZE = 12,
    ARRAY_PREFIX_SIZE = 8,
    OP_SIZE = 8,
};

static void prepare_response(const struct tgc_protocol_packet *request,
                             struct tgc_protocol_packet *response)
{
    response->header.version = TGC_PROTOCOL_VERSION;
    response->header.kind = TGC_PROTOCOL_RESPONSE;
    response->header.opcode = request->header.opcode;
    response->header.request_id = request->header.request_id;
    response->header.payload_length = 0;
    response->header.result = 0;
}

static int dispatch_semget(struct tgc_sem_store *store,
                           const struct tgc_protocol_packet *request)
{
    if (request->header.payload_length != SEMGET_SIZE) {
        return -EMSGSIZE;
    }
    int32_t key = tgc_wire_get_i32(request->payload);
    int32_t nsems = tgc_wire_get_i32(request->payload + 4);
    uint32_t flags = tgc_wire_get_u32(request->payload + 8);
    const uint32_t allowed_flags = TGC_IPC_CREAT | TGC_IPC_EXCL | 0777U;
    if ((flags & ~allowed_flags) != 0U) {
        return -EINVAL;
    }
    return tgc_sem_store_get(store, key, nsems, (int)flags);
}

static int dispatch_semnum(const struct tgc_sem_store *store,
                           const struct tgc_protocol_packet *request,
                           uint16_t opcode)
{
    if (request->header.payload_length != SEMNUM_SIZE) {
        return -EMSGSIZE;
    }
    int semid = tgc_wire_get_i32(request->payload);
    uint32_t semnum = tgc_wire_get_u32(request->payload + 4);
    if (opcode == TGC_OPCODE_GETVAL) {
        return tgc_sem_store_getval(store, semid, semnum);
    }
    return tgc_sem_store_getpid(store, semid, semnum);
}

static int dispatch_setval(struct tgc_sem_store *store,
                           const struct tgc_protocol_packet *request,
                           int32_t actor_pid)
{
    if (request->header.payload_length != SETVAL_SIZE) {
        return -EMSGSIZE;
    }
    int semid = tgc_wire_get_i32(request->payload);
    uint32_t semnum = tgc_wire_get_u32(request->payload + 4);
    uint32_t value = tgc_wire_get_u32(request->payload + 8);
    return tgc_sem_store_setval(store, semid, semnum, value, actor_pid);
}

static int dispatch_getall(const struct tgc_sem_store *store,
                           const struct tgc_protocol_packet *request,
                           struct tgc_protocol_packet *response)
{
    if (request->header.payload_length != SEMNUM_SIZE) {
        return -EMSGSIZE;
    }
    int semid = tgc_wire_get_i32(request->payload);
    uint32_t count = tgc_wire_get_u32(request->payload + 4);
    if (count == 0 || count > TGC_SEM_MAX_PER_SET) {
        return -EINVAL;
    }
    uint16_t values[TGC_SEM_MAX_PER_SET];
    int result = tgc_sem_store_getall(store, semid, values, count);
    if (result != 0) {
        return result;
    }
    tgc_wire_put_u32(response->payload, count);
    for (uint32_t i = 0; i < count; ++i) {
        tgc_wire_put_u16(response->payload + 4 + (i * 2), values[i]);
    }
    response->header.payload_length = 4 + (count * 2);
    return 0;
}

static int dispatch_setall(struct tgc_sem_store *store,
                           const struct tgc_protocol_packet *request,
                           int32_t actor_pid)
{
    if (request->header.payload_length < ARRAY_PREFIX_SIZE) {
        return -EMSGSIZE;
    }
    int semid = tgc_wire_get_i32(request->payload);
    uint32_t count = tgc_wire_get_u32(request->payload + 4);
    if (count == 0 || count > TGC_SEM_MAX_PER_SET ||
        request->header.payload_length != ARRAY_PREFIX_SIZE + (count * 2)) {
        return -EMSGSIZE;
    }
    uint16_t values[TGC_SEM_MAX_PER_SET];
    for (uint32_t i = 0; i < count; ++i) {
        values[i] = tgc_wire_get_u16(request->payload + 8 + (i * 2));
    }
    return tgc_sem_store_setall(store, semid, values, count, actor_pid);
}

static int dispatch_semop(struct tgc_sem_store *store,
                          const struct tgc_protocol_packet *request,
                          int32_t actor_pid)
{
    if (request->header.payload_length < ARRAY_PREFIX_SIZE) {
        return -EMSGSIZE;
    }
    int semid = tgc_wire_get_i32(request->payload);
    uint32_t count = tgc_wire_get_u32(request->payload + 4);
    if (count == 0 || count > TGC_SEM_MAX_PER_SET ||
        request->header.payload_length != ARRAY_PREFIX_SIZE + (count * OP_SIZE)) {
        return -EMSGSIZE;
    }
    struct tgc_sem_op operations[TGC_SEM_MAX_PER_SET];
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *wire = request->payload + 8 + (i * OP_SIZE);
        if (tgc_wire_get_u16(wire + 6) != 0) {
            return -EPROTO;
        }
        operations[i].sem_num = tgc_wire_get_u16(wire);
        operations[i].sem_op = tgc_wire_get_i16(wire + 2);
        operations[i].sem_flg = tgc_wire_get_u16(wire + 4);
    }
    return tgc_sem_store_tryop(store, semid, operations, count, actor_pid);
}

int tgc_broker_dispatch(struct tgc_sem_store *store,
                        const struct tgc_protocol_packet *request,
                        int32_t actor_pid,
                        struct tgc_protocol_packet *response)
{
    if (store == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    if (actor_pid <= 0) {
        return -EINVAL;
    }
    if (request->header.version != TGC_PROTOCOL_VERSION ||
        request->header.kind != TGC_PROTOCOL_REQUEST ||
        request->header.result != 0 ||
        request->header.payload_length > TGC_PROTOCOL_MAX_PAYLOAD) {
        return -EPROTO;
    }
    prepare_response(request, response);

    int result = 0;
    switch (request->header.opcode) {
    case TGC_OPCODE_PING:
        result = request->header.payload_length == 0 ? 0 : -EMSGSIZE;
        break;
    case TGC_OPCODE_SEMGET:
        result = dispatch_semget(store, request);
        break;
    case TGC_OPCODE_REMOVE:
        result = request->header.payload_length == SEMID_SIZE
                     ? tgc_sem_store_remove(
                           store, tgc_wire_get_i32(request->payload))
                     : -EMSGSIZE;
        break;
    case TGC_OPCODE_GETVAL:
    case TGC_OPCODE_GETPID:
        result = dispatch_semnum(store, request, request->header.opcode);
        break;
    case TGC_OPCODE_SETVAL:
        result = dispatch_setval(store, request, actor_pid);
        break;
    case TGC_OPCODE_GETALL:
        result = dispatch_getall(store, request, response);
        break;
    case TGC_OPCODE_SETALL:
        result = dispatch_setall(store, request, actor_pid);
        break;
    case TGC_OPCODE_SEMOP:
        result = dispatch_semop(store, request, actor_pid);
        break;
    default:
        result = -ENOSYS;
        break;
    }
    response->header.result = result;
    if (result != 0) {
        response->header.payload_length = 0;
    }
    return 0;
}
