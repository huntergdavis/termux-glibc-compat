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
    TIMED_OP_PREFIX_SIZE = 16,
    METADATA_SIZE = 56,
    SETMETA_SIZE = 16,
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
                           const struct tgc_protocol_packet *request,
                           struct tgc_sem_identity identity)
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
    return tgc_sem_store_get_as(store, key, nsems, (int)flags, identity);
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
    if (opcode == TGC_OPCODE_GETPID) {
        return tgc_sem_store_getpid(store, semid, semnum);
    }
    return tgc_sem_store_get_wait_count(
        store, semid, semnum, opcode == TGC_OPCODE_GETZCNT);
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
                          int32_t actor_pid,
                          struct tgc_protocol_packet *response)
{
    uint32_t prefix_size = request->header.opcode == TGC_OPCODE_SEMTIMEDOP
                               ? TIMED_OP_PREFIX_SIZE
                               : ARRAY_PREFIX_SIZE;
    if (request->header.payload_length < prefix_size) {
        return -EMSGSIZE;
    }
    int semid = tgc_wire_get_i32(request->payload);
    uint32_t count = tgc_wire_get_u32(request->payload + 4);
    if (count == 0 || count > TGC_SEM_MAX_PER_SET ||
        request->header.payload_length != prefix_size + (count * OP_SIZE)) {
        return -EMSGSIZE;
    }
    if (request->header.opcode == TGC_OPCODE_SEMTIMEDOP &&
        tgc_wire_get_i64(request->payload + 8) < 0) {
        return -EINVAL;
    }
    struct tgc_sem_op operations[TGC_SEM_MAX_PER_SET];
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *wire = request->payload + prefix_size + (i * OP_SIZE);
        if (tgc_wire_get_u16(wire + 6) != 0) {
            return -EPROTO;
        }
        operations[i].sem_num = tgc_wire_get_u16(wire);
        operations[i].sem_op = tgc_wire_get_i16(wire + 2);
        operations[i].sem_flg = tgc_wire_get_u16(wire + 4);
    }
    struct tgc_sem_wait_reason reason = {0};
    int result = tgc_sem_store_tryop_detail(store, semid, operations, count,
                                            actor_pid, &reason);
    if (result == TGC_SEM_OP_BLOCKED) {
        tgc_wire_put_u16(response->payload, reason.sem_num);
        tgc_wire_put_u16(response->payload + 2, reason.wait_for_zero);
        tgc_wire_put_u32(response->payload + 4, 0);
        response->header.payload_length = 8;
    }
    return result;
}

static void encode_metadata(struct tgc_protocol_packet *response,
                            const struct tgc_sem_metadata *metadata)
{
    tgc_wire_put_i32(response->payload, metadata->key);
    tgc_wire_put_u32(response->payload + 4, metadata->uid);
    tgc_wire_put_u32(response->payload + 8, metadata->gid);
    tgc_wire_put_u32(response->payload + 12, metadata->cuid);
    tgc_wire_put_u32(response->payload + 16, metadata->cgid);
    tgc_wire_put_u32(response->payload + 20, metadata->mode);
    tgc_wire_put_u32(response->payload + 24, metadata->nsems);
    tgc_wire_put_u32(response->payload + 28, 0);
    tgc_wire_put_i64(response->payload + 32, metadata->otime);
    tgc_wire_put_i64(response->payload + 40, metadata->ctime);
    tgc_wire_put_u32(response->payload + 48, metadata->sequence);
    tgc_wire_put_u32(response->payload + 52, 0);
    response->header.payload_length = METADATA_SIZE;
}

static int dispatch_stat(const struct tgc_sem_store *store,
                         const struct tgc_protocol_packet *request,
                         struct tgc_protocol_packet *response)
{
    if (request->header.payload_length != SEMID_SIZE) {
        return -EMSGSIZE;
    }
    struct tgc_sem_metadata metadata;
    int result = tgc_sem_store_get_metadata(
        store, tgc_wire_get_i32(request->payload), &metadata);
    if (result != 0) {
        return result;
    }
    encode_metadata(response, &metadata);
    return 0;
}

static int dispatch_stat_index(const struct tgc_sem_store *store,
                               const struct tgc_protocol_packet *request,
                               struct tgc_protocol_packet *response)
{
    if (request->header.payload_length != SEMID_SIZE) {
        return -EMSGSIZE;
    }
    struct tgc_sem_metadata metadata;
    int result = tgc_sem_store_stat_index(
        store, tgc_wire_get_u32(request->payload), &metadata);
    if (result < 0) {
        return result;
    }
    encode_metadata(response, &metadata);
    return result;
}

static int dispatch_setmeta(struct tgc_sem_store *store,
                            const struct tgc_protocol_packet *request,
                            struct tgc_sem_identity actor)
{
    if (request->header.payload_length != SETMETA_SIZE) {
        return -EMSGSIZE;
    }
    return tgc_sem_store_set_metadata(
        store, tgc_wire_get_i32(request->payload),
        tgc_wire_get_u32(request->payload + 4),
        tgc_wire_get_u32(request->payload + 8),
        tgc_wire_get_u32(request->payload + 12), actor);
}

static int dispatch_info(const struct tgc_sem_store *store,
                         const struct tgc_protocol_packet *request,
                         struct tgc_protocol_packet *response)
{
    if (request->header.payload_length != 4) {
        return -EMSGSIZE;
    }
    uint32_t dynamic = tgc_wire_get_u32(request->payload);
    if (dynamic > 1) {
        return -EINVAL;
    }
    struct tgc_sem_info info;
    int result = tgc_sem_store_info(store, (int)dynamic, &info);
    if (result < 0) {
        return result;
    }
    const int32_t fields[] = {
        info.semmap, info.semmni, info.semmns, info.semmnu, info.semmsl,
        info.semopm, info.semume, info.semusz, info.semvmx, info.semaem,
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        tgc_wire_put_i32(response->payload + (i * 4), fields[i]);
    }
    response->header.payload_length = (uint32_t)sizeof(fields);
    return result;
}

int tgc_broker_dispatch(struct tgc_sem_store *store,
                        const struct tgc_protocol_packet *request,
                        struct tgc_broker_actor actor,
                        struct tgc_protocol_packet *response)
{
    if (store == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    if (actor.pid <= 0) {
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
        result = dispatch_semget(store, request, actor.identity);
        break;
    case TGC_OPCODE_REMOVE:
        result = request->header.payload_length == SEMID_SIZE
                     ? tgc_sem_store_remove(
                           store, tgc_wire_get_i32(request->payload))
                     : -EMSGSIZE;
        break;
    case TGC_OPCODE_GETVAL:
    case TGC_OPCODE_GETPID:
    case TGC_OPCODE_GETNCNT:
    case TGC_OPCODE_GETZCNT:
        result = dispatch_semnum(store, request, request->header.opcode);
        break;
    case TGC_OPCODE_SETVAL:
        result = dispatch_setval(store, request, actor.pid);
        break;
    case TGC_OPCODE_GETALL:
        result = dispatch_getall(store, request, response);
        break;
    case TGC_OPCODE_SETALL:
        result = dispatch_setall(store, request, actor.pid);
        break;
    case TGC_OPCODE_SEMOP:
    case TGC_OPCODE_SEMTIMEDOP:
        result = dispatch_semop(store, request, actor.pid, response);
        break;
    case TGC_OPCODE_STAT:
        result = dispatch_stat(store, request, response);
        break;
    case TGC_OPCODE_STAT_INDEX:
        result = dispatch_stat_index(store, request, response);
        break;
    case TGC_OPCODE_SETMETA:
        result = dispatch_setmeta(store, request, actor.identity);
        break;
    case TGC_OPCODE_INFO:
        result = dispatch_info(store, request, response);
        break;
    default:
        result = -ENOSYS;
        break;
    }
    response->header.result = result;
    if (result < 0) {
        response->header.payload_length = 0;
    }
    return 0;
}
