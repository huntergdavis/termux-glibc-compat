#ifndef TGCOMPAT_PROTOCOL_H
#define TGCOMPAT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

enum {
    TGC_PROTOCOL_MAGIC = 0x43534754,
    TGC_PROTOCOL_VERSION = 1,
    TGC_PROTOCOL_HEADER_SIZE = 24,
    TGC_PROTOCOL_MAX_PAYLOAD = 8192,
    TGC_PROTOCOL_REQUEST = 1,
    TGC_PROTOCOL_RESPONSE = 2,
};

enum tgc_protocol_opcode {
    TGC_OPCODE_PING = 1,
    TGC_OPCODE_SEMGET = 2,
    TGC_OPCODE_REMOVE = 3,
    TGC_OPCODE_GETVAL = 4,
    TGC_OPCODE_SETVAL = 5,
    TGC_OPCODE_GETPID = 6,
    TGC_OPCODE_GETALL = 7,
    TGC_OPCODE_SETALL = 8,
    TGC_OPCODE_SEMOP = 9,
};

struct tgc_protocol_header {
    uint16_t version;
    uint16_t kind;
    uint16_t opcode;
    uint32_t request_id;
    uint32_t payload_length;
    int32_t result;
};

struct tgc_protocol_packet {
    struct tgc_protocol_header header;
    uint8_t payload[TGC_PROTOCOL_MAX_PAYLOAD];
};

/*
 * Wire integers are explicitly little-endian. The fixed 24-byte header is:
 * magic:u32, version:u16, kind:u16, opcode:u16, reserved:u16,
 * request_id:u32, payload_length:u32, result:i32.
 *
 * Request result fields and all reserved fields must be zero. Responses carry
 * the state-core return value in result. GETALL is the only version-1 response
 * with a payload: count:u32 followed by count value:u16 entries. Operations
 * record the caller PID from authenticated socket credentials, never from a
 * client-supplied field.
 */
int tgc_protocol_encode_header(
    uint8_t output[TGC_PROTOCOL_HEADER_SIZE],
    const struct tgc_protocol_header *header);
int tgc_protocol_decode_header(
    const uint8_t input[TGC_PROTOCOL_HEADER_SIZE],
    struct tgc_protocol_header *header);

void tgc_wire_put_u16(uint8_t *output, uint16_t value);
void tgc_wire_put_u32(uint8_t *output, uint32_t value);
void tgc_wire_put_i16(uint8_t *output, int16_t value);
void tgc_wire_put_i32(uint8_t *output, int32_t value);
uint16_t tgc_wire_get_u16(const uint8_t *input);
uint32_t tgc_wire_get_u32(const uint8_t *input);
int16_t tgc_wire_get_i16(const uint8_t *input);
int32_t tgc_wire_get_i32(const uint8_t *input);

#endif
