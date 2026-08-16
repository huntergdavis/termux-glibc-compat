#include <tgcompat/protocol.h>

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
            return EXIT_FAILURE;                                                \
        }                                                                       \
    } while (0)

int main(void)
{
    struct tgc_protocol_header header = {
        .version = TGC_PROTOCOL_VERSION,
        .kind = TGC_PROTOCOL_REQUEST,
        .opcode = TGC_OPCODE_SEMGET,
        .request_id = 0x78563412U,
        .payload_length = 12,
        .result = 0,
    };
    uint8_t wire[TGC_PROTOCOL_HEADER_SIZE] = {0};
    CHECK(tgc_protocol_encode_header(wire, &header) == 0);
    const uint8_t expected[] = {
        0x54, 0x47, 0x53, 0x43, 0x01, 0x00, 0x01, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78,
        0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    CHECK(memcmp(wire, expected, sizeof(expected)) == 0);

    struct tgc_protocol_header decoded = {0};
    CHECK(tgc_protocol_decode_header(wire, &decoded) == 0);
    CHECK(decoded.version == header.version);
    CHECK(decoded.kind == header.kind);
    CHECK(decoded.opcode == header.opcode);
    CHECK(decoded.request_id == header.request_id);
    CHECK(decoded.payload_length == header.payload_length);
    CHECK(decoded.result == 0);

    wire[0] ^= 1;
    CHECK(tgc_protocol_decode_header(wire, &decoded) == -EPROTO);
    wire[0] ^= 1;
    wire[10] = 1;
    CHECK(tgc_protocol_decode_header(wire, &decoded) == -EPROTO);
    wire[10] = 0;
    tgc_wire_put_u32(wire + 16, TGC_PROTOCOL_MAX_PAYLOAD + 1U);
    CHECK(tgc_protocol_decode_header(wire, &decoded) == -EMSGSIZE);

    uint8_t integers[28] = {0};
    tgc_wire_put_u16(integers, 0xabcdU);
    tgc_wire_put_i16(integers + 2, -1234);
    tgc_wire_put_u32(integers + 4, 0x89abcdefU);
    tgc_wire_put_i32(integers + 8, -123456789);
    tgc_wire_put_u64(integers + 12, UINT64_C(0xfedcba9876543210));
    tgc_wire_put_i64(integers + 20, INT64_C(-1234567890123456));
    CHECK(tgc_wire_get_u16(integers) == 0xabcdU);
    CHECK(tgc_wire_get_i16(integers + 2) == -1234);
    CHECK(tgc_wire_get_u32(integers + 4) == 0x89abcdefU);
    CHECK(tgc_wire_get_i32(integers + 8) == -123456789);
    CHECK(tgc_wire_get_u64(integers + 12) ==
          UINT64_C(0xfedcba9876543210));
    CHECK(tgc_wire_get_i64(integers + 20) ==
          INT64_C(-1234567890123456));

    header.opcode = TGC_OPCODE_SEMTIMEDOP;
    header.payload_length = 16;
    CHECK(tgc_protocol_encode_header(wire, &header) == 0);

    puts("protocol: PASS");
    return EXIT_SUCCESS;
}
