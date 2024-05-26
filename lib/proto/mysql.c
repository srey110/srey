#include "proto/mysql.h"
#include "proto/protos.h"
#include "algo/sha1.h"
#include "utils.h"
#include "srey/trigger.h"

typedef enum parse_status {
    INIT = 0,
    HANDSHAKE,
    COMMAND
}parse_status;
//typedef struct mysql_
//{
//
//}option;
#define MYSQL_HEAD_LENS 4
static _handshaked_push _hs_push;

void mysql_pkfree(void *pack) {
    
}
void mysql_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }

}
static int32_t _mysql_parse_head(buffer_ctx *buf, size_t *payload_lens, int32_t *sequence_id) {
    size_t size = buffer_size(buf);
    if (size < MYSQL_HEAD_LENS) {
        return ERR_FAILED;
    }
    char head[MYSQL_HEAD_LENS];
    ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer failed.");
    *payload_lens = (size_t)head[0] + ((size_t)head[1] << 8) +  ((size_t)head[2] << 16);
    if (size < *payload_lens + sizeof(head)) {
        return ERR_FAILED;
    }
    *sequence_id = head[3];
    ASSERTAB(sizeof(head) == buffer_drain(buf, sizeof(head)), "drain buffer failed.");
    return ERR_OK;
}
static void _mysql_auth(ev_ctx *ev, SOCKET fd, uint64_t skid, ud_cxt *ud, char *scramble, size_t lens) {
    unsigned char stage1[SHA1_BLOCK_SIZE];
    unsigned char stage2[SHA1_BLOCK_SIZE];
    unsigned char stage3[SHA1_BLOCK_SIZE];
    sha1_ctx sha1;
    sha1_init(&sha1);
    sha1_update(&sha1, "12345678", 8);
    sha1_final(&sha1, stage1);
    sha1_init(&sha1);
    sha1_update(&sha1, stage1, sizeof(stage1));
    sha1_final(&sha1, stage2);
    sha1_init(&sha1);
    sha1_update(&sha1, scramble, lens);
    sha1_update(&sha1, stage2, sizeof(stage2));
    sha1_final(&sha1, stage3);
    for (int32_t i = 0; i < sizeof(stage2); i++) {
        stage2[i] = stage1[i] ^ stage3[i];
    }
    /*char hex[HEX_ENSIZE(sizeof(stage2))];
    tohex(stage2, sizeof(stage2), hex);*/

}
static void _mysql_handshake(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *closefd) {
    size_t payload_lens;
    int32_t sequence_id;
    if (ERR_OK != _mysql_parse_head(buf, &payload_lens, &sequence_id)) {
        return;
    }
    char *p, payload[128];
    ASSERTAB(payload_lens == buffer_remove(buf, payload, payload_lens), "copy buffer failed.");
    p = payload;
    if (0x0a != p[0]) {//protocol version
        *closefd = 1;
        return;
    }
    p += (strlen(p) + 1 + 4);
    char scramble[8 + 13];
    memcpy(scramble, p, 8);
    p += 16;
    int32_t lens = (*p) - 8 - 1;
    p += 11;
    memcpy(scramble + 8, p, lens);
    _mysql_auth(ev, fd, skid, ud, scramble, 8 + lens);
}
void *mysql_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid,
    buffer_ctx *buf, ud_cxt *ud, int32_t *closefd, int32_t *slice) {
    mysql_pack_ctx *pack = NULL;
    switch (ud->status) {
    case INIT:
        _mysql_handshake(ev, fd, skid, buf, ud, closefd);
        break;
    case HANDSHAKE:
        break;
    case COMMAND:
        break;
    default:
        *closefd = 1;
        break;
    }
    return pack;
}
//SOCKET mysql_connect(task_ctx *task, const char *ip, uint16_t port,
//    const char *user, const char *password, const char *database, const char *charset, uint64_t *skid) {
//    
//}
void _mysql_init(void *hspush) {
    _hs_push = (_handshaked_push)hspush;
}
