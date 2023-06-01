#include "proto/websock.h"

#define WEBSOCK_VER  13
#define PLLENS_125 125
#define PLLENS_126 126
#define PLLENS_127 127
#define HEAD_BASE_LEN 6
#define HEAD_EXT16_LEN 8
#define HEAD_EXT64_LEN 14

typedef enum  websock_proto {
    CONTINUE = 0x00,
    TEXT = 0x01,
    BINARY = 0x02,
    CLOSE = 0x08,
    PING = 0x09,
    PONG = 0x0A
}websock_proto;
typedef struct websock_frame {
    char fin;
    char rsv1;
    char rsv2;
    char rsv3;
    char mask;
    uint16_t proto;
    unsigned char pllen;
}websock_frame;

void *websock_unpack(buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    return NULL;
}
void *websock_pack(void *data, size_t lens, size_t *size) {
    return NULL;
}
