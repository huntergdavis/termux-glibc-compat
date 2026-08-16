#include <tgcompat/client.h>
#include <tgcompat/protocol.h>
#include <tgcompat/transport.h>

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static void request_init(struct tgc_protocol_packet *request, uint16_t opcode,
                         uint32_t payload_length)
{
    request->header.version = TGC_PROTOCOL_VERSION;
    request->header.kind = TGC_PROTOCOL_REQUEST;
    request->header.opcode = opcode;
    request->header.request_id = 0;
    request->header.payload_length = payload_length;
    request->header.result = 0;
}

int tgc_client_init(struct tgc_client *client, const char *socket_path)
{
    if (client == NULL || socket_path == NULL) {
        return -EINVAL;
    }
    size_t length = strlen(socket_path);
    if (socket_path[0] != '/' || length == 0 ||
        length >= sizeof(client->socket_path)) {
        return length >= sizeof(client->socket_path) ? -ENAMETOOLONG : -EINVAL;
    }
    memset(client, 0, sizeof(*client));
    client->socket_fd = -1;
    memcpy(client->socket_path, socket_path, length + 1);
    return 0;
}

void tgc_client_close(struct tgc_client *client)
{
    if (client == NULL) {
        return;
    }
    if (client->socket_fd >= 0) {
        (void)close(client->socket_fd);
    }
    client->socket_fd = -1;
    client->connection_pid = 0;
}

static int ensure_connection(struct tgc_client *client)
{
    pid_t current_pid = getpid();
    if (client->socket_fd >= 0 && client->connection_pid == current_pid) {
        return 0;
    }
    tgc_client_close(client);
    int socket_fd = tgc_transport_connect(client->socket_path, geteuid());
    if (socket_fd < 0) {
        return socket_fd;
    }
    client->socket_fd = socket_fd;
    client->connection_pid = current_pid;
    return 0;
}

static int exchange(struct tgc_client *client,
                    struct tgc_protocol_packet *request,
                    struct tgc_protocol_packet *response)
{
    if (client == NULL || request == NULL || response == NULL) {
        return -EINVAL;
    }
    int result = ensure_connection(client);
    if (result != 0) {
        return result;
    }
    client->next_request_id += 1;
    if (client->next_request_id == 0) {
        client->next_request_id = 1;
    }
    request->header.request_id = client->next_request_id;
    result = tgc_transport_send(client->socket_fd, request);
    if (result == 0) {
        result = tgc_transport_receive(client->socket_fd, response);
    }
    if (result != 0) {
        tgc_client_close(client);
        return result == TGC_TRANSPORT_EOF ? -ECONNRESET : result;
    }
    if (response->header.version != TGC_PROTOCOL_VERSION ||
        response->header.kind != TGC_PROTOCOL_RESPONSE ||
        response->header.opcode != request->header.opcode ||
        response->header.request_id != request->header.request_id ||
        (response->header.result < 0 && response->header.payload_length != 0)) {
        tgc_client_close(client);
        return -EPROTO;
    }
    return 0;
}

static int scalar_call(struct tgc_client *client,
                       struct tgc_protocol_packet *request)
{
    struct tgc_protocol_packet response;
    int result = exchange(client, request, &response);
    if (result != 0) {
        return result;
    }
    if (response.header.payload_length != 0) {
        tgc_client_close(client);
        return -EPROTO;
    }
    return response.header.result;
}

int tgc_client_ping(struct tgc_client *client)
{
    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_PING, 0);
    return scalar_call(client, &request);
}

int tgc_client_semget(struct tgc_client *client, int32_t key, int nsems,
                      int flags)
{
    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_SEMGET, 12);
    tgc_wire_put_i32(request.payload, key);
    tgc_wire_put_i32(request.payload + 4, nsems);
    tgc_wire_put_u32(request.payload + 8, (uint32_t)flags);
    return scalar_call(client, &request);
}

int tgc_client_remove(struct tgc_client *client, int semid)
{
    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_REMOVE, 4);
    tgc_wire_put_i32(request.payload, semid);
    return scalar_call(client, &request);
}

static int semnum_call(struct tgc_client *client, uint16_t opcode, int semid,
                       size_t semnum)
{
    if (semnum > UINT32_MAX) {
        return -EINVAL;
    }
    struct tgc_protocol_packet request;
    request_init(&request, opcode, 8);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, (uint32_t)semnum);
    return scalar_call(client, &request);
}

int tgc_client_getval(struct tgc_client *client, int semid, size_t semnum)
{
    return semnum_call(client, TGC_OPCODE_GETVAL, semid, semnum);
}

int tgc_client_setval(struct tgc_client *client, int semid, size_t semnum,
                      unsigned int value)
{
    if (semnum > UINT32_MAX) {
        return -EINVAL;
    }
    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_SETVAL, 12);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, (uint32_t)semnum);
    tgc_wire_put_u32(request.payload + 8, value);
    return scalar_call(client, &request);
}

int tgc_client_getpid(struct tgc_client *client, int semid, size_t semnum)
{
    return semnum_call(client, TGC_OPCODE_GETPID, semid, semnum);
}

int tgc_client_getncnt(struct tgc_client *client, int semid, size_t semnum)
{
    return semnum_call(client, TGC_OPCODE_GETNCNT, semid, semnum);
}

int tgc_client_getzcnt(struct tgc_client *client, int semid, size_t semnum)
{
    return semnum_call(client, TGC_OPCODE_GETZCNT, semid, semnum);
}

int tgc_client_getall(struct tgc_client *client, int semid, uint16_t *values,
                      size_t count)
{
    if (values == NULL || count == 0 || count > TGC_SEM_MAX_PER_SET) {
        return -EINVAL;
    }
    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_GETALL, 8);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, (uint32_t)count);

    struct tgc_protocol_packet response;
    int result = exchange(client, &request, &response);
    if (result != 0) {
        return result;
    }
    if (response.header.result != 0) {
        return response.header.result;
    }
    if (response.header.payload_length != 4 + (count * 2) ||
        tgc_wire_get_u32(response.payload) != count) {
        tgc_client_close(client);
        return -EPROTO;
    }
    for (size_t i = 0; i < count; ++i) {
        values[i] = tgc_wire_get_u16(response.payload + 4 + (i * 2));
    }
    return 0;
}

int tgc_client_setall(struct tgc_client *client, int semid,
                      const uint16_t *values, size_t count)
{
    if (values == NULL || count == 0 || count > TGC_SEM_MAX_PER_SET) {
        return -EINVAL;
    }
    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_SETALL, (uint32_t)(8 + (count * 2)));
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, (uint32_t)count);
    for (size_t i = 0; i < count; ++i) {
        tgc_wire_put_u16(request.payload + 8 + (i * 2), values[i]);
    }
    return scalar_call(client, &request);
}

static int semop_call(struct tgc_client *client, int semid,
                      const struct tgc_sem_op *operations, size_t count,
                      int timed, int64_t timeout_nanoseconds)
{
    if (operations == NULL || count == 0 || count > TGC_SEM_MAX_PER_SET ||
        (timed != 0 && timeout_nanoseconds < 0)) {
        return -EINVAL;
    }
    struct tgc_protocol_packet request;
    uint32_t prefix_size = timed != 0 ? 16U : 8U;
    request_init(&request,
                 timed != 0 ? TGC_OPCODE_SEMTIMEDOP : TGC_OPCODE_SEMOP,
                 (uint32_t)(prefix_size + (count * 8)));
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, (uint32_t)count);
    if (timed != 0) {
        tgc_wire_put_i64(request.payload + 8, timeout_nanoseconds);
    }
    for (size_t i = 0; i < count; ++i) {
        uint8_t *wire = request.payload + prefix_size + (i * 8);
        tgc_wire_put_u16(wire, operations[i].sem_num);
        tgc_wire_put_i16(wire + 2, operations[i].sem_op);
        tgc_wire_put_u16(wire + 4, operations[i].sem_flg);
        tgc_wire_put_u16(wire + 6, 0);
    }
    int result = scalar_call(client, &request);
    return result == TGC_SEM_OP_BLOCKED ? -EPROTO : result;
}

int tgc_client_semop(struct tgc_client *client, int semid,
                     const struct tgc_sem_op *operations, size_t count)
{
    return semop_call(client, semid, operations, count, 0, 0);
}

int tgc_client_semtimedop(struct tgc_client *client, int semid,
                          const struct tgc_sem_op *operations, size_t count,
                          int64_t timeout_nanoseconds)
{
    return semop_call(client, semid, operations, count, 1,
                      timeout_nanoseconds);
}

int tgc_client_stat(struct tgc_client *client, int semid,
                    struct tgc_sem_metadata *metadata)
{
    if (metadata == NULL) {
        return -EINVAL;
    }
    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_STAT, 4);
    tgc_wire_put_i32(request.payload, semid);
    struct tgc_protocol_packet response;
    int result = exchange(client, &request, &response);
    if (result != 0) {
        return result;
    }
    if (response.header.result != 0) {
        return response.header.result;
    }
    if (response.header.payload_length != 56 ||
        tgc_wire_get_u32(response.payload + 28) != 0 ||
        tgc_wire_get_u32(response.payload + 52) != 0) {
        tgc_client_close(client);
        return -EPROTO;
    }
    *metadata = (struct tgc_sem_metadata){
        .key = tgc_wire_get_i32(response.payload),
        .uid = tgc_wire_get_u32(response.payload + 4),
        .gid = tgc_wire_get_u32(response.payload + 8),
        .cuid = tgc_wire_get_u32(response.payload + 12),
        .cgid = tgc_wire_get_u32(response.payload + 16),
        .mode = tgc_wire_get_u32(response.payload + 20),
        .nsems = tgc_wire_get_u32(response.payload + 24),
        .otime = tgc_wire_get_i64(response.payload + 32),
        .ctime = tgc_wire_get_i64(response.payload + 40),
        .sequence = tgc_wire_get_u32(response.payload + 48),
    };
    return 0;
}

int tgc_client_set_metadata(struct tgc_client *client, int semid,
                            uint32_t uid, uint32_t gid, uint32_t mode)
{
    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_SETMETA, 16);
    tgc_wire_put_i32(request.payload, semid);
    tgc_wire_put_u32(request.payload + 4, uid);
    tgc_wire_put_u32(request.payload + 8, gid);
    tgc_wire_put_u32(request.payload + 12, mode);
    return scalar_call(client, &request);
}

int tgc_client_info(struct tgc_client *client, int dynamic,
                    struct tgc_sem_info *info)
{
    if (info == NULL || (dynamic != 0 && dynamic != 1)) {
        return -EINVAL;
    }
    struct tgc_protocol_packet request;
    request_init(&request, TGC_OPCODE_INFO, 4);
    tgc_wire_put_u32(request.payload, (uint32_t)dynamic);
    struct tgc_protocol_packet response;
    int result = exchange(client, &request, &response);
    if (result != 0) {
        return result;
    }
    if (response.header.result < 0) {
        return response.header.result;
    }
    if (response.header.payload_length != 40) {
        tgc_client_close(client);
        return -EPROTO;
    }
    int32_t *fields[] = {
        &info->semmap, &info->semmni, &info->semmns, &info->semmnu,
        &info->semmsl, &info->semopm, &info->semume, &info->semusz,
        &info->semvmx, &info->semaem,
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        *fields[i] = tgc_wire_get_i32(response.payload + (i * 4));
    }
    return response.header.result;
}
