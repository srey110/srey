#include "proto/websock.h"
#include "proto/protos.h"
#include "proto/http.h"
#include "service/srey.h"
#include "crypto/base64.h"
#include "crypto/sha1.h"
#include "netutils.h"

typedef enum parse_status {
    INIT = 0,
    START,
    DATA
}parse_status;
typedef struct websock_pack_ctx {
    char fin;
    char proto;
    char mask;
    char key[4];
    size_t remain;
    size_t dlens;
    char data[0];
}websock_pack_ctx;

#define HEAD_LESN    2
#define SIGNKEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
static char _mask_key[4 + 1] = { 0 };

static inline http_header_ctx *_websock_handshake_svcheck(struct http_pack_ctx *hpack) {
    buf_ctx *status = http_status(hpack);
    if (!buf_icompare(&status[0], "get", strlen("get"))) {
        return NULL;
    }
    http_header_ctx *head;
    http_header_ctx *sign = NULL;
    uint8_t conn = 0, upgrade = 0, version = 0, checked = 0;
    size_t cnt = http_nheader(hpack);
    for (size_t i = 0; i < cnt; i++) {
        head = http_header_at(hpack, i);
        switch (tolower(*((char *)head->key.data))) {
        case 'c':
            if (0 == conn
                && ERR_OK == _http_check_keyval(head, "connection", "upgrade")) {
                conn = 1;
            }
            break;
        case 'u':
            if (0 == upgrade
                && ERR_OK == _http_check_keyval(head, "upgrade", "websocket")) {
                upgrade = 1;
            }
            break;
        case 's':
            checked = 0;
            if (0 == version
                && ERR_OK == _http_check_keyval(head, "sec-websocket-version", "13")) {
                version = 1;
                checked = 1;
            }
            if (0 == checked
                && NULL == sign
                && ERR_OK == _http_check_keyval(head, "sec-websocket-key", NULL)) {
                sign = head;
            }
            break;
        default:
            break;
        }
        if (0 != conn
            && 0 != upgrade
            && 0 != version
            && NULL != sign) {
            break;
        }
    }
    if (0 == conn
        || 0 == upgrade
        || 0 == version) {
        return NULL;
    }
    return sign;
}
static inline void _websock_handshake_server(ev_ctx *ev, SOCKET fd, uint64_t skid, struct http_pack_ctx *hpack, ud_cxt *ud, int32_t *closefd) {
    http_header_ctx *signstr = _websock_handshake_svcheck(hpack);
    if (NULL == signstr) {
        *closefd = 1;
        push_handshaked(fd, skid, ud, closefd, ERR_FAILED);
        return;
    }
    unsigned char *key;
    size_t klens = strlen(SIGNKEY);
    size_t lens = klens + signstr->value.lens;
    MALLOC(key, lens);
    memcpy(key, signstr->value.data, signstr->value.lens);
    memcpy(key + signstr->value.lens, SIGNKEY, klens);
    sha1_ctx sha1;
    unsigned char sha1str[SHA1_BLOCK_SIZE];
    sha1_init(&sha1);
    sha1_update(&sha1, key, lens);
    sha1_final(&sha1, sha1str);
    FREE(key);
    char b64[B64EN_BLOCK_SIZE(sizeof(sha1str))];
    b64_encode((char *)sha1str, sizeof(sha1str), b64);
    static const char *fmt = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n";
    char *rsp = formatv(fmt, b64);
    ud->status = START;
    ev_send(ev, fd, skid, rsp, strlen(rsp), 0);
    push_handshaked(fd, skid, ud, closefd, ERR_OK);
}
static inline int32_t _websock_handshake_clientckstatus(struct http_pack_ctx *hpack) {
    buf_ctx *status = http_status(hpack);
    if (!buf_compare(&status[1], "101", strlen("101"))) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
static inline http_header_ctx *websock_client_checkhs(struct http_pack_ctx *hpack) {
    if (ERR_OK != _websock_handshake_clientckstatus(hpack)) {
        return NULL;
    }
    http_header_ctx *head;
    http_header_ctx *sign = NULL;
    uint8_t conn = 0, upgrade = 0;
    size_t cnt = http_nheader(hpack);
    for (size_t i = 0; i < cnt; i++) {
        head = http_header_at(hpack, i);
        switch (tolower(*((char*)head->key.data))) {
        case 'c':
            if (0 == conn
                && ERR_OK == _http_check_keyval(head, "connection", "upgrade")) {
                conn = 1;
            }
            break;
        case 'u':
            if (0 == upgrade
                && ERR_OK == _http_check_keyval(head, "upgrade", "websocket")) {
                upgrade = 1;
            }
            break;
        case 's':
            if (NULL == sign
                && ERR_OK == _http_check_keyval(head, "sec-websocket-accept", NULL)) {
                sign = head;
            }
            break;
        default:
            break;
        }
        if (0 != conn
            && 0 != upgrade
            && NULL != sign) {
            break;
        }
    }
    if (0 == conn
        || 0 == upgrade
        || NULL == sign) {
        return NULL;
    }
    return sign;
}
static inline void _websock_handshake_client(struct http_pack_ctx *hpack, SOCKET fd, uint64_t skid, ud_cxt *ud, int32_t *closefd) {
    http_header_ctx *signstr = websock_client_checkhs(hpack);
    if (NULL == signstr) {
        *closefd = 1;
        push_handshaked(fd, skid, ud, closefd, ERR_FAILED);
        return;
    }
    ud->status = START;
    push_handshaked(fd, skid, ud, closefd, ERR_OK);
}
static inline void _websock_handshake(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, int32_t *closefd) {
    int32_t status;
    struct http_pack_ctx *hpack = _http_parsehead(buf, &status, closefd);
    if (NULL == hpack) {
        return;
    }
    if (0 != status) {
        *closefd = 1;
        push_handshaked(fd, skid, ud, closefd, ERR_FAILED);
        http_pkfree(hpack);
        return;
    }
    buf_ctx *hstatus = http_status(hpack);
    if (!buf_icompare(&hstatus[0], "get", strlen("get"))) {
        _websock_handshake_client(hpack, fd, skid, ud, closefd);
    } else {
        _websock_handshake_server(ev, fd, skid, hpack, ud, closefd);
    }
    http_pkfree(hpack);
}
static inline websock_pack_ctx *_websock_parse_data(buffer_ctx *buf, ud_cxt *ud, int32_t *closefd, int32_t *slice) {
    websock_pack_ctx *pack = ud->extra;
    if (pack->remain > buffer_size(buf)) {
        return NULL;
    }
    if (pack->remain > 0) {
        if (0 == pack->mask) {
            ASSERTAB(pack->dlens == buffer_copyout(buf, 0, pack->data, pack->dlens), "copy buffer failed.");
        } else {
            ASSERTAB(sizeof(pack->key) == buffer_copyout(buf, 0, pack->key, sizeof(pack->key)), "copy buffer failed.");
            ASSERTAB(pack->dlens == buffer_copyout(buf, sizeof(pack->key), pack->data, pack->dlens), "copy buffer failed.");
            for (size_t i = 0; i < pack->dlens; i++) {
                pack->data[i] = pack->data[i] ^ pack->key[i % 4];
            }
        }
        ASSERTAB(pack->remain == buffer_drain(buf, pack->remain), "drain buffer failed.");
    }
    ud->extra = NULL;
    ud->status = START;
    //起始帧:FIN为0,opcode非0 中间帧:FIN为0,opcode为0 结束帧:FIN为1,opcode为0
    if (0 == pack->fin 
        && 0 != pack->proto) {
        *slice = SLICE_START;
    } else if (0 == pack->fin
        && 0 == pack->proto) {
        *slice = SLICE;
    } else if (1 == pack->fin
        && 0 == pack->proto) {
        *slice = SLICE_END;
    }
    return pack;
}
static inline websock_pack_ctx *_websock_parse_pllens(buffer_ctx *buf, size_t blens, 
    uint8_t mask, uint8_t payloadlen, int32_t *closefd) {
    websock_pack_ctx *pack = NULL;
    if (payloadlen <= 125) {
        if (PACK_TOO_LONG(payloadlen)) {
            *closefd = 1;
            return NULL;
        }
        MALLOC(pack, sizeof(websock_pack_ctx) + payloadlen);
        pack->dlens = payloadlen;
        if (0 == mask) {
            pack->remain = payloadlen;
        } else {
            pack->remain = sizeof(pack->key) + payloadlen;
        }
        ASSERTAB(HEAD_LESN == buffer_drain(buf, HEAD_LESN), "drain buffer failed.");
    } else if (126 == payloadlen) {
        uint16_t pllens;
        size_t atlest = HEAD_LESN + sizeof(pllens);
        if (blens < atlest) {
            return NULL;
        }
        ASSERTAB(sizeof(pllens) == buffer_copyout(buf, HEAD_LESN, &pllens, sizeof(pllens)), "copy buffer failed.");
        pllens = ntohs(pllens);
        if (PACK_TOO_LONG(pllens)) {
            *closefd = 1;
            return NULL;
        }
        MALLOC(pack, sizeof(websock_pack_ctx) + pllens);
        pack->dlens = pllens;
        if (0 == mask) {
            pack->remain = pllens;
        } else {
            pack->remain = sizeof(pack->key) + pllens;
        }
        ASSERTAB(atlest == buffer_drain(buf, atlest), "drain buffer failed.");
    } else if (127 == payloadlen) {
        uint64_t pllens;
        size_t atlest = HEAD_LESN + sizeof(pllens);
        if (blens < atlest) {
            return NULL;
        }
        ASSERTAB(sizeof(pllens) == buffer_copyout(buf, HEAD_LESN, &pllens, sizeof(pllens)), "copy buffer failed.");
        pllens = ntohll(pllens);
        if (PACK_TOO_LONG(pllens)) {
            *closefd = 1;
            return NULL;
        }
        MALLOC(pack, sizeof(websock_pack_ctx) + (size_t)pllens);
        pack->dlens = (size_t)pllens;
        if (0 == mask) {
            pack->remain = (size_t)pllens;
        } else {
            pack->remain = sizeof(pack->key) + (size_t)pllens;
        }
        ASSERTAB(atlest == buffer_drain(buf, atlest), "drain buffer failed.");
    } else {
        *closefd = 1;
        return NULL;
    }
    return pack;
}
static inline websock_pack_ctx *_websock_parse_head(buffer_ctx *buf, ud_cxt *ud, int32_t *closefd, int32_t *slice) {
    size_t blens = buffer_size(buf);
    if (blens < HEAD_LESN) {
        return NULL;
    }
    uint8_t head[HEAD_LESN];
    ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer failed.");
    if (0 != ((head[0] & 0x40) >> 6)
        || 0 != ((head[0] & 0x20) >> 5)
        || 0 != ((head[0] & 0x10) >> 4)) {
        *closefd = 1;
        return NULL;
    }
    uint8_t fin = (head[0] & 0x80) >> 7;
    uint8_t proto = head[0] & 0xf;
    uint8_t mask = (head[1] & 0x80) >> 7;
    if (0 == ud->client
        && 0 == mask) {
        *closefd = 1;
        return NULL;
    }
    uint8_t payloadlen = head[1] & 0x7f;
    websock_pack_ctx *pack = _websock_parse_pllens(buf, blens, mask, payloadlen, closefd);
    if (NULL == pack) {
        return NULL;
    }
    pack->fin = fin;
    pack->proto = proto;
    pack->mask = mask;
    ud->extra = pack;
    ud->status = DATA;
    return _websock_parse_data(buf, ud, closefd, slice);
}
websock_pack_ctx *websock_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid,
    buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd, int32_t *slice) {
    websock_pack_ctx *pack = NULL;
    switch (ud->status) {
    case INIT:
        _websock_handshake(ev, fd, skid, buf, ud, closefd);
        break;
    case START:
        pack = _websock_parse_head(buf, ud, closefd, slice);
        break;
    case DATA:
        pack = _websock_parse_data(buf, ud, closefd, slice);
        break;
    default:
        break;
    }
    return pack;
}
static inline size_t _websock_create_callens(char key[4], size_t dlens) {
    size_t size = HEAD_LESN + dlens;
    if (dlens >= 126) {
        if (dlens > 0xffff) {
            size += sizeof(uint64_t);
        } else {
            size += sizeof(uint16_t);
        }
    }
    if (NULL != key) {
        size += 4;
    }
    return size;
}
static inline void *_websock_create_pack(uint8_t fin, uint8_t proto, char key[4], void *data, size_t dlens, size_t *size) {
    *size = _websock_create_callens(key, dlens);
    char *frame;
    MALLOC(frame, *size);
    frame[0] = 0;
    frame[1] = 0;
    if (0 != fin) {
        BIT_SET(frame[0], 0x80);
    }
    BIT_SET(frame[0], (proto & 0xf));
    if (NULL != key) {
        BIT_SET(frame[1], 0x80);
    }
    size_t offset = HEAD_LESN;
    if (dlens <= 125) {
        BIT_SET(frame[1], dlens);
    } else if (dlens <= 0xffff) {
        BIT_SET(frame[1], 126);
        uint16_t pllens = htons((u_short)dlens);
        memcpy(frame + offset, &pllens, sizeof(pllens));
        offset += sizeof(pllens);
    } else {
        BIT_SET(frame[1], 127);
        uint64_t pllens = htonll(dlens);
        memcpy(frame + offset, &pllens, sizeof(pllens));
        offset += sizeof(pllens);
    }
    if (NULL != key) {
        memcpy(frame + offset, key, 4);
        offset += 4;
        if (NULL != data) {
            memcpy(frame + offset, data, dlens);
            char *tmp = frame + offset;
            for (size_t i = 0; i < dlens; i++) {
                tmp[i] = tmp[i] ^ key[i % 4];
            }
        }
    } else {
        if (NULL != data) {
            memcpy(frame + offset, data, dlens);
        }
    }
    return frame;
}
static inline void _websock_control_frame(ev_ctx *ev, SOCKET fd, uint64_t skid, uint8_t proto, char key[4]) {
    size_t flens;
    void *frame = _websock_create_pack(1, proto, key, NULL, 0, &flens);
    ev_send(ev, fd, skid, frame, flens, 0);
}
void websock_ping(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask) {
    if (0 == mask) {
        _websock_control_frame(ev, fd, skid, WBSK_PING, NULL);
    } else {
        _websock_control_frame(ev, fd, skid, WBSK_PING, _mask_key);
    }
}
void websock_pong(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask) {
    if (0 == mask) {
        _websock_control_frame(ev, fd, skid, WBSK_PONG, NULL);
    } else {
        _websock_control_frame(ev, fd, skid, WBSK_PONG, _mask_key);
    }
}
void websock_close(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask) {
    if (0 == mask) {
        _websock_control_frame(ev, fd, skid, WBSK_CLOSE, NULL);
    } else {
        _websock_control_frame(ev, fd, skid, WBSK_CLOSE, _mask_key);
    }
    
}
void websock_text(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask,
    int32_t fin, void *data, size_t dlens) {
    if (0 == mask) {
        size_t flens;
        void *frame = _websock_create_pack(fin, WBSK_TEXT, NULL, data, dlens, &flens);
        ev_send(ev, fd, skid, frame, flens, 0);
    } else {
        size_t flens;
        void *frame = _websock_create_pack(fin, WBSK_TEXT, _mask_key, data, dlens, &flens);
        ev_send(ev, fd, skid, frame, flens, 0);
    }
}
void websock_binary(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask,
    int32_t fin, void *data, size_t dlens) {
    if (0 == mask) {
        size_t flens;
        void *frame = _websock_create_pack(fin, WBSK_BINARY, NULL, data, dlens, &flens);
        ev_send(ev, fd, skid, frame, flens, 0);
    } else {
        size_t flens;
        void *frame = _websock_create_pack(fin, WBSK_BINARY, _mask_key, data, dlens, &flens);
        ev_send(ev, fd, skid, frame, flens, 0);
    }
}
void websock_continuation(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t mask,
    int32_t fin, void *data, size_t dlens) {
    if (0 == mask) {
        size_t flens;
        void *frame = _websock_create_pack(fin, WBSK_CONTINUE, NULL, data, dlens, &flens);
        ev_send(ev, fd, skid, frame, flens, 0);
    } else {
        size_t flens;
        void *frame = _websock_create_pack(fin, WBSK_CONTINUE, _mask_key, data, dlens, &flens);
        ev_send(ev, fd, skid, frame, flens, 0);
    }
}
int32_t websock_pack_fin(websock_pack_ctx *pack) {
    return pack->fin;
}
int32_t websock_pack_proto(websock_pack_ctx *pack) {
    return pack->proto;
}
char *websock_pack_data(websock_pack_ctx *pack, size_t *lens) {
    *lens = pack->dlens;
    return pack->data;
}
char *websock_handshake_pack(const char *host) {
    char rdstr[8 + 1];
    randstr(rdstr, sizeof(rdstr) - 1);
    char b64[B64EN_BLOCK_SIZE(sizeof(rdstr) - 1)];
    b64_encode(rdstr, sizeof(rdstr) - 1, b64);
    char *data;
    if (NULL != host) {
        const char *fmt = "GET / HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade,Keep-Alive\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n";
        data = formatv(fmt, host, b64);
    } else {
        const char *fmt = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade,Keep-Alive\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n";
        data = formatv(fmt, b64);
    }
    return data;
}
void _websock_init_key(void) {
    randstr(_mask_key, sizeof(_mask_key) - 1);
}
