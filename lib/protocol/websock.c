#include "protocol/websock.h"
#include "protocol/protos.h"
#include "protocol/http.h"
#include "crypt/base64.h"
#include "crypt/digest.h"
#include "utils/utils.h"

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
static char _hs_key[B64EN_SIZE(8)] = { 0 };
static char _hs_sign[B64EN_SIZE(SHA1_BLOCK_SIZE)] = { 0 };
static _handshaked_push _hs_push;

void _websock_pkfree(void *data) {
    FREE(data);
}
static http_header_ctx *_websock_handshake_svcheck(struct http_pack_ctx *hpack) {
    buf_ctx *status = http_status(hpack);
    if (!buf_icompare(&status[0], "get", strlen("get"))) {
        return NULL;
    }
    http_header_ctx *head;
    http_header_ctx *sign = NULL;
    uint8_t conn = 0, upgrade = 0, version = 0, checked = 0;
    uint32_t cnt = http_nheader(hpack);
    for (uint32_t i = 0; i < cnt; i++) {
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
static void _websock_sign(char *key, size_t klens, char b64[B64EN_SIZE(SHA1_BLOCK_SIZE)]) {
    char *signstr;
    size_t slens = strlen(SIGNKEY);
    size_t lens = klens + slens;
    MALLOC(signstr, lens);
    memcpy(signstr, key, klens);
    memcpy(signstr + klens, SIGNKEY, slens);
    char sha1str[SHA1_BLOCK_SIZE];
    digest_ctx digest;
    digest_init(&digest, DG_SHA1);
    digest_update(&digest, signstr, lens);
    digest_final(&digest, sha1str);
    FREE(signstr);
    bs64_encode(sha1str, sizeof(sha1str), b64);
}
static void _websock_handshake_server(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    ud_cxt *ud, struct http_pack_ctx *hpack, int32_t *status) {
    http_header_ctx *signstr = _websock_handshake_svcheck(hpack);
    if (NULL == signstr) {
        BIT_SET(*status, PROTO_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return;
    }
    size_t lens = 0;
    char *sechead = http_header(hpack, "Sec-WebSocket-Protocol", &lens);
    char b64[B64EN_SIZE(SHA1_BLOCK_SIZE)];
    _websock_sign(signstr->value.data, signstr->value.lens, b64);
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_resp(&bwriter, 101);
    http_pack_head(&bwriter, "Upgrade", "websocket");
    http_pack_head(&bwriter, "Connection", "Upgrade");
    http_pack_head(&bwriter, "Sec-WebSocket-Accept", b64);
    char *secproto = NULL;
    if (NULL != sechead
        && 0 != lens) {
        MALLOC(secproto, lens + 1);
        memcpy(secproto, sechead, lens);
        secproto[lens] = '\0';
        http_pack_head(&bwriter, "Sec-WebSocket-Protocol", secproto);
    }
    http_pack_end(&bwriter);
    ev_send(ev, fd, skid, bwriter.data, bwriter.offset, 0);
    if (ERR_OK != _hs_push(fd, skid, client, ud, ERR_OK, secproto, lens)) {
        BIT_SET(*status, PROTO_ERROR);
        FREE(secproto);
    } else {
        ud->status = START;
    }
}
static int32_t _websock_handshake_clientckstatus(struct http_pack_ctx *hpack) {
    buf_ctx *status = http_status(hpack);
    if (!buf_compare(&status[1], "101", strlen("101"))) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
static http_header_ctx *websock_client_checkhs(struct http_pack_ctx *hpack) {
    if (ERR_OK != _websock_handshake_clientckstatus(hpack)) {
        return NULL;
    }
    http_header_ctx *head;
    http_header_ctx *sign = NULL;
    uint8_t conn = 0, upgrade = 0;
    uint32_t cnt = http_nheader(hpack);
    for (uint32_t i = 0; i < cnt; i++) {
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
static void _websock_handshake_client(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud,
    struct http_pack_ctx *hpack, int32_t *status) {
    http_header_ctx *signstr = websock_client_checkhs(hpack);
    if (NULL == signstr) {
        BIT_SET(*status, PROTO_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return;
    }
    if (!buf_compare(&signstr->value, _hs_sign, strlen(_hs_sign))){
        BIT_SET(*status, PROTO_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return;
    }
    size_t lens = 0;
    char *sechead = http_header(hpack, "Sec-WebSocket-Protocol", &lens);
    char *secproto = NULL;
    if (NULL != sechead
        && 0 != lens) {
        MALLOC(secproto, lens + 1);
        memcpy(secproto, sechead, lens);
        secproto[lens] = '\0';
    }
    if (ERR_OK != _hs_push(fd, skid, client, ud, ERR_OK, secproto, lens)) {
        BIT_SET(*status, PROTO_ERROR);
        FREE(secproto);
    } else {
        ud->status = START;
    }
}
static void _websock_handshake(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t transfer;
    struct http_pack_ctx *hpack = _http_parsehead(buf, &transfer, status);
    if (NULL == hpack) {
        return;
    }
    if (0 != transfer) {
        BIT_SET(*status, PROTO_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        _http_pkfree(hpack);
        return;
    }
    if (client) {
        _websock_handshake_client(fd, skid, client, ud, hpack, status);
    } else {
        _websock_handshake_server(ev, fd, skid, client, ud, hpack, status);
    }
    _http_pkfree(hpack);
}
static websock_pack_ctx *_websock_parse_data(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    websock_pack_ctx *pack = ud->extra;
    if (pack->remain > buffer_size(buf)) {
        BIT_SET(*status, PROTO_MOREDATA);
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
        BIT_SET(*status, PROTO_SLICE_START);
    } else if (0 == pack->fin
        && 0 == pack->proto) {
        BIT_SET(*status, PROTO_SLICE);
    } else if (1 == pack->fin
        && 0 == pack->proto) {
        BIT_SET(*status, PROTO_SLICE_END);
    }
    return pack;
}
static websock_pack_ctx *_websock_parse_pllens(buffer_ctx *buf, size_t blens, 
    uint8_t mask, uint8_t payloadlen, int32_t *status) {
    websock_pack_ctx *pack = NULL;
    if (payloadlen <= 125) {
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
            BIT_SET(*status, PROTO_ERROR);
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
            BIT_SET(*status, PROTO_ERROR);
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
        BIT_SET(*status, PROTO_ERROR);
        return NULL;
    }
    return pack;
}
static websock_pack_ctx *_websock_parse_head(buffer_ctx *buf, int32_t client, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < HEAD_LESN) {
        BIT_SET(*status, PROTO_MOREDATA);
        return NULL;
    }
    uint8_t head[HEAD_LESN];
    ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer failed.");
    if (0 != ((head[0] & 0x40) >> 6)
        || 0 != ((head[0] & 0x20) >> 5)
        || 0 != ((head[0] & 0x10) >> 4)) {
        BIT_SET(*status, PROTO_ERROR);
        return NULL;
    }
    uint8_t fin = (head[0] & 0x80) >> 7;
    uint8_t proto = head[0] & 0xf;
    uint8_t mask = (head[1] & 0x80) >> 7;
    if (!client
        && 0 == mask) {
        BIT_SET(*status, PROTO_ERROR);
        return NULL;
    }
    uint8_t payloadlen = head[1] & 0x7f;
    websock_pack_ctx *pack = _websock_parse_pllens(buf, blens, mask, payloadlen, status);
    if (NULL == pack) {
        return NULL;
    }
    pack->fin = fin;
    pack->proto = proto;
    pack->mask = mask;
    ud->extra = pack;
    ud->status = DATA;
    return _websock_parse_data(buf, ud, status);
}
websock_pack_ctx *websock_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    websock_pack_ctx *pack = NULL;
    switch (ud->status) {
    case INIT:
        _websock_handshake(ev, fd, skid, client, buf, ud, status);
        break;
    case START:
        pack = _websock_parse_head(buf, client, ud, status);
        break;
    case DATA:
        pack = _websock_parse_data(buf, ud, status);
        break;
    default:
        break;
    }
    return pack;
}
static size_t _websock_create_callens(char key[4], size_t dlens) {
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
static void *_websock_create_pack(uint8_t fin, uint8_t proto, char key[4], void *data, size_t dlens, size_t *size) {
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
        memcpy(frame + offset, &pllens, sizeof(uint16_t));
        offset += sizeof(pllens);
    } else {
        BIT_SET(frame[1], 127);
        uint64_t pllens = htonll((uint64_t)dlens);
        memcpy(frame + offset, &pllens, sizeof(uint64_t));
        offset += sizeof(uint64_t);
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
void *websock_ping(int32_t mask, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(1, WBSK_PING, NULL, NULL, 0, size);
    } else {
        return _websock_create_pack(1, WBSK_PING, _mask_key, NULL, 0, size);
    }
}
void *websock_pong(int32_t mask, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(1, WBSK_PONG, NULL, NULL, 0, size);
    } else {
        return _websock_create_pack(1, WBSK_PONG, _mask_key, NULL, 0, size);
    }
}
void *websock_close(int32_t mask, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(1, WBSK_CLOSE, NULL, NULL, 0, size);
    } else {
        return _websock_create_pack(1, WBSK_CLOSE, _mask_key, NULL, 0, size);
    }
}
void *websock_text(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(fin, WBSK_TEXT, NULL, data, dlens, size);
    } else {
        return _websock_create_pack(fin, WBSK_TEXT, _mask_key, data, dlens, size);
    }
}
void *websock_binary(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(fin, WBSK_BINARY, NULL, data, dlens, size);
    } else {
        return _websock_create_pack(fin, WBSK_BINARY, _mask_key, data, dlens, size);
    }
}
void *websock_continuation(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(fin, WBSK_CONTINUE, NULL, data, dlens, size);
    } else {
        return _websock_create_pack(fin, WBSK_CONTINUE, _mask_key, data, dlens, size);
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
char *websock_handshake_pack(const char *host, const char *secproto) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_req(&bwriter, "GET", "/");
    if (NULL != host
        && 0 != strlen(host)) {
        http_pack_head(&bwriter, "Host", host);
    }
    http_pack_head(&bwriter, "Upgrade", "websocket");
    http_pack_head(&bwriter, "Connection", "Upgrade,Keep-Alive");
    http_pack_head(&bwriter, "Sec-WebSocket-Key", _hs_key);
    http_pack_head(&bwriter, "Sec-WebSocket-Version", "13");
    if (NULL != secproto
        && 0 != strlen(host)) {
        http_pack_head(&bwriter, "Sec-WebSocket-Protocol", secproto);
    }
    http_pack_end(&bwriter);
    bwriter.data[bwriter.offset] = '\0';
    return bwriter.data;
}
void _websock_init(void *hspush) {
    _hs_push = (_handshaked_push)hspush;
    randstr(_mask_key, sizeof(_mask_key) - 1);
    char key[8 + 1];
    randstr(key, sizeof(key) - 1);
    bs64_encode(key, sizeof(key) - 1, _hs_key);
    _websock_sign(_hs_key, strlen(_hs_key), _hs_sign);
}
