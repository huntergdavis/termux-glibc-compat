#include <tgcompat/protocol.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

void tgc_wire_put_u16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)(value & 0xffU);
    output[1] = (uint8_t)(value >> 8);
}

void tgc_wire_put_u32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)(value & 0xffU);
    output[1] = (uint8_t)((value >> 8) & 0xffU);
    output[2] = (uint8_t)((value >> 16) & 0xffU);
    output[3] = (uint8_t)(value >> 24);
}

void tgc_wire_put_i16(uint8_t *output, int16_t value)
{
    tgc_wire_put_u16(output, (uint16_t)value);
}

void tgc_wire_put_i32(uint8_t *output, int32_t value)
{
    tgc_wire_put_u32(output, (uint32_t)value);
}

void tgc_wire_put_u64(uint8_t *output, uint64_t value)
{
    tgc_wire_put_u32(output, (uint32_t)value);
    tgc_wire_put_u32(output + 4, (uint32_t)(value >> 32));
}

void tgc_wire_put_i64(uint8_t *output, int64_t value)
{
    tgc_wire_put_u64(output, (uint64_t)value);
}

uint16_t tgc_wire_get_u16(const uint8_t *input)
{
    return (uint16_t)((uint16_t)input[0] | ((uint16_t)input[1] << 8));
}

uint32_t tgc_wire_get_u32(const uint8_t *input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

int16_t tgc_wire_get_i16(const uint8_t *input)
{
    return (int16_t)tgc_wire_get_u16(input);
}

int32_t tgc_wire_get_i32(const uint8_t *input)
{
    return (int32_t)tgc_wire_get_u32(input);
}

uint64_t tgc_wire_get_u64(const uint8_t *input)
{
    return (uint64_t)tgc_wire_get_u32(input) |
           ((uint64_t)tgc_wire_get_u32(input + 4) << 32);
}

int64_t tgc_wire_get_i64(const uint8_t *input)
{
    return (int64_t)tgc_wire_get_u64(input);
}

static int header_is_valid(const struct tgc_protocol_header *header)
{
    if (header == NULL || header->version != TGC_PROTOCOL_VERSION) {
        return -EPROTO;
    }
    if (header->kind != TGC_PROTOCOL_REQUEST &&
        header->kind != TGC_PROTOCOL_RESPONSE) {
        return -EPROTO;
    }
    if (header->opcode < TGC_OPCODE_PING ||
        header->opcode > TGC_OPCODE_INFO) {
        return -EPROTO;
    }
    if (header->payload_length > TGC_PROTOCOL_MAX_PAYLOAD) {
        return -EMSGSIZE;
    }
    if (header->kind == TGC_PROTOCOL_REQUEST && header->result != 0) {
        return -EPROTO;
    }
    return 0;
}

int tgc_protocol_encode_header(
    uint8_t output[TGC_PROTOCOL_HEADER_SIZE],
    const struct tgc_protocol_header *header)
{
    if (output == NULL || header == NULL) {
        return -EINVAL;
    }
    int result = header_is_valid(header);
    if (result != 0) {
        return result;
    }

    tgc_wire_put_u32(output, TGC_PROTOCOL_MAGIC);
    tgc_wire_put_u16(output + 4, header->version);
    tgc_wire_put_u16(output + 6, header->kind);
    tgc_wire_put_u16(output + 8, header->opcode);
    tgc_wire_put_u16(output + 10, 0);
    tgc_wire_put_u32(output + 12, header->request_id);
    tgc_wire_put_u32(output + 16, header->payload_length);
    tgc_wire_put_i32(output + 20, header->result);
    return 0;
}

int tgc_protocol_decode_header(
    const uint8_t input[TGC_PROTOCOL_HEADER_SIZE],
    struct tgc_protocol_header *header)
{
    if (input == NULL || header == NULL) {
        return -EINVAL;
    }
    if (tgc_wire_get_u32(input) != TGC_PROTOCOL_MAGIC ||
        tgc_wire_get_u16(input + 10) != 0) {
        return -EPROTO;
    }

    struct tgc_protocol_header decoded = {
        .version = tgc_wire_get_u16(input + 4),
        .kind = tgc_wire_get_u16(input + 6),
        .opcode = tgc_wire_get_u16(input + 8),
        .request_id = tgc_wire_get_u32(input + 12),
        .payload_length = tgc_wire_get_u32(input + 16),
        .result = tgc_wire_get_i32(input + 20),
    };
    int result = header_is_valid(&decoded);
    if (result != 0) {
        return result;
    }
    *header = decoded;
    return 0;
}
