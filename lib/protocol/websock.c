#include "protocol/websock.h"
#include "protocol/prots.h"
#include "protocol/http.h"
#include "protocol/mqtt/mqtt.h"
#include "crypt/base64.h"
#include "crypt/digest.h"
#include "utils/utils.h"

typedef enum parse_status {
    INIT = 0,
    START,
    DATA
}parse_status;
typedef struct websock_pack_ctx {
    int8_t fin;
    int8_t prot;
    int8_t mask;
    pack_type secprot;
    void *secpack;
    char key[4];
    size_t remain;
    size_t dlens;
    char data[0];
}websock_pack_ctx;
typedef struct websock_ctx {
    int8_t slice;
    pack_type secprot;//子协议
    buffer_ctx *buf;
    ud_cxt *ud;
    websock_pack_ctx *pack;
}websock_ctx;

#define HEAD_LESN    2
#define SIGNKEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
static char _mask_key[4 + 1] = { 0 };
static char _hs_key[B64EN_SIZE(8)] = { 0 };
static char _hs_sign[B64EN_SIZE(SHA1_BLOCK_SIZE)] = { 0 };
static _handshaked_push _hs_push;

void _websock_pkfree(void *data) {
    if (NULL == data) {
        return;
    }
    websock_pack_ctx *pack = (websock_pack_ctx *)data;
    prots_pkfree(pack->secprot, pack->secpack);
    FREE(data);
}
void _websock_udfree(ud_cxt *ud) {
    if (NULL == ud->extra) {
        return;
    }
    websock_ctx *ws = (websock_ctx *)ud->extra;
    _websock_pkfree(ws->pack);
    prots_udfree(ws->ud);
    FREE(ws->ud);
    if (NULL != ws->buf) {
        buffer_free(ws->buf);
        FREE(ws->buf);
    }
    FREE(ws);
    ud->extra = NULL;
}
void _websock_secextra(ud_cxt *ud, void *val) {
    if (NULL == ud->extra) {
        LOG_WARN("set second ud_cxt extra data error.");
        return;
    }
    websock_ctx *ws = (websock_ctx *)ud->extra;
    if (NULL == ws->ud) {
        LOG_WARN("set second ud_cxt extra data error.");
        return;
    }
    ws->ud->extra = val;
}
static int32_t _websock_sec_prot(char *secprot, pack_type *sectype) {
    if (0 == _memicmp(secprot, "mqtt", strlen("mqtt"))) {
        *sectype = PACK_MQTT;
        return ERR_OK;
    }
    return ERR_FAILED;
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
static int32_t _websock_handshake_server(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    ud_cxt *ud, struct http_pack_ctx *hpack, int32_t *status, pack_type *sectype) {
    http_header_ctx *signstr = _websock_handshake_svcheck(hpack);
    if (NULL == signstr) {
        BIT_SET(*status, PROT_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return ERR_FAILED;
    }
    size_t lens = 0;
    char *sechead = http_header(hpack, "Sec-WebSocket-Protocol", &lens);
    char *secprot = NULL;
    if (NULL != sechead
        && 0 != lens) {
        MALLOC(secprot, lens + 1);
        memcpy(secprot, sechead, lens);
        secprot[lens] = '\0';
        if (ERR_OK != _websock_sec_prot(secprot, sectype)) {
            _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
            FREE(secprot);
            return ERR_FAILED;
        }
    }
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_resp(&bwriter, 101);
    http_pack_head(&bwriter, "Upgrade", "websocket");
    http_pack_head(&bwriter, "Connection", "Upgrade");
    char b64[B64EN_SIZE(SHA1_BLOCK_SIZE)];
    _websock_sign(signstr->value.data, signstr->value.lens, b64);
    http_pack_head(&bwriter, "Sec-WebSocket-Accept", b64);
    if (NULL != secprot) {
        http_pack_head(&bwriter, "Sec-WebSocket-Protocol", secprot);
    }
    http_pack_end(&bwriter);
    ud->status = START;
    ev_send(ev, fd, skid, bwriter.data, bwriter.offset, 0);
    if (ERR_OK != _hs_push(fd, skid, client, ud, ERR_OK, secprot, lens)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    } else {
        return ERR_OK;
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
static int32_t _websock_handshake_client(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud,
    struct http_pack_ctx *hpack, int32_t *status, pack_type *sectype) {
    http_header_ctx *signstr = websock_client_checkhs(hpack);
    if (NULL == signstr) {
        BIT_SET(*status, PROT_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return ERR_FAILED;
    }
    if (!buf_compare(&signstr->value, _hs_sign, strlen(_hs_sign))){
        BIT_SET(*status, PROT_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return ERR_FAILED;
    }
    size_t lens = 0;
    char *sechead = http_header(hpack, "Sec-WebSocket-Protocol", &lens);
    char *secprot = NULL;
    if (NULL != sechead
        && 0 != lens) {
        MALLOC(secprot, lens + 1);
        memcpy(secprot, sechead, lens);
        secprot[lens] = '\0';
        if (ERR_OK != _websock_sec_prot(secprot, sectype)) {
            _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
            FREE(secprot);
            return ERR_FAILED;
        }
    }
    if (ERR_OK != _hs_push(fd, skid, client, ud, ERR_OK, secprot, lens)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    } else {
        ud->status = START;
        return ERR_OK;
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
        BIT_SET(*status, PROT_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        _http_pkfree(hpack);
        return;
    }
    int32_t rtn;
    pack_type sectype = PACK_NONE;
    if (client) {
        rtn = _websock_handshake_client(fd, skid, client, ud, hpack, status, &sectype);
    } else {
        rtn = _websock_handshake_server(ev, fd, skid, client, ud, hpack, status, &sectype);
    }
    if (ERR_OK == rtn) {
        CALLOC(ud->extra, 1, sizeof(websock_ctx));
        websock_ctx *ws = (websock_ctx *)ud->extra;
        ws->secprot = sectype;
        if (PACK_NONE != sectype) {//有子协议
            MALLOC(ws->buf, sizeof(buffer_ctx));
            buffer_init(ws->buf);
            CALLOC(ws->ud, 1, sizeof(ud_cxt));
            ws->ud->pktype = sectype;
            ws->ud->name = ud->name;
            ws->ud->data = ud->data;
        }
    }
    _http_pkfree(hpack);
}
static void _websock_mqtt_buffree(void *buf) {
    websock_pack_ctx *pack = UPCAST(buf, websock_pack_ctx, data);
    _websock_pkfree(pack);
}
static websock_pack_ctx *_websock_sec_mqtt(websock_ctx *ws, websock_pack_ctx *pack, int32_t client, int32_t *status) {
    if (WS_BINARY != pack->prot
        && WS_CONTINUE != pack->prot) {
        BIT_SET(*status, PROT_ERROR);
        _websock_pkfree(pack);
        return NULL;
    }
    buffer_external(ws->buf, pack->data, pack->dlens, _websock_mqtt_buffree);
    struct mqtt_pack_ctx *mpack = mqtt_unpack(client, ws->buf, ws->ud, status);
    if (NULL == mpack) {
        return NULL;
    }
    websock_pack_ctx *rtn;
    CALLOC(rtn, 1, sizeof(websock_pack_ctx));
    rtn->fin = 1;
    rtn->prot = WS_BINARY;
    rtn->secprot = ws->secprot;
    rtn->secpack = mpack;
    return rtn;
}
static websock_pack_ctx *_websock_sec_unpack(websock_ctx *ws, websock_pack_ctx *pack, int32_t client, int32_t *status) {
    websock_pack_ctx *rtn = NULL;
    switch (ws->secprot) {
    case PACK_MQTT:
        rtn = _websock_sec_mqtt(ws, pack, client, status);
        break;
    default:
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    //移除标记,可以继续循环
    if (BIT_CHECK(*status, PROT_MOREDATA)) {
        BIT_REMOVE(*status, PROT_MOREDATA);
    }
    return rtn;
}
static websock_pack_ctx *_websock_parse_data(buffer_ctx *buf, int32_t client, ud_cxt *ud, int32_t *status) {
    websock_ctx *ws = (websock_ctx *)ud->extra;
    websock_pack_ctx *pack = ws->pack;
    if (pack->remain > buffer_size(buf)) {
        BIT_SET(*status, PROT_MOREDATA);
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
    //起始帧:FIN为0,opcode非0 中间帧:FIN为0,opcode为0 结束帧:FIN为1,opcode为0
    if (0 == pack->fin 
        && 0 != pack->prot) {
        if (0 != ws->slice) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        ws->slice = 1;
        BIT_SET(*status, PROT_SLICE_START);
    } else if (0 == pack->fin
        && 0 == pack->prot) {
        BIT_SET(*status, PROT_SLICE);
    } else if (1 == pack->fin
        && 0 == pack->prot) {
        ws->slice = 0;
        BIT_SET(*status, PROT_SLICE_END);
    }
    if (WS_CLOSE == pack->prot) {
        BIT_SET(*status, PROT_CLOSE);
    }
    ws->pack = NULL;
    ud->status = START;
    if (PACK_NONE != ws->secprot//有子协议
        && pack->dlens > 0//排除空包
        && (WS_CONTINUE == pack->prot
            || WS_TEXT == pack->prot
            || WS_BINARY == pack->prot)) {
        return _websock_sec_unpack(ws, pack, client, status);
    } else {
        return pack;
    }
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
            BIT_SET(*status, PROT_ERROR);
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
            BIT_SET(*status, PROT_ERROR);
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
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    return pack;
}
static websock_pack_ctx *_websock_parse_head(buffer_ctx *buf, int32_t client, ud_cxt *ud, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < HEAD_LESN) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    uint8_t head[HEAD_LESN];
    ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer failed.");
    if (0 != ((head[0] & 0x40) >> 6)
        || 0 != ((head[0] & 0x20) >> 5)
        || 0 != ((head[0] & 0x10) >> 4)) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    uint8_t fin = (head[0] & 0x80) >> 7;
    uint8_t prot = head[0] & 0xf;
    uint8_t mask = (head[1] & 0x80) >> 7;
    if (!client
        && 0 == mask) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    uint8_t payloadlen = head[1] & 0x7f;
    websock_pack_ctx *pack = _websock_parse_pllens(buf, blens, mask, payloadlen, status);
    if (NULL == pack) {
        return NULL;
    }
    pack->fin = fin;
    pack->prot = prot;
    pack->mask = mask;
    websock_ctx *ws = (websock_ctx *)ud->extra;
    pack->secprot = ws->secprot;
    pack->secpack = NULL;
    ws->pack = pack;
    ud->status = DATA;
    return _websock_parse_data(buf, client, ud, status);
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
        pack = _websock_parse_data(buf, client, ud, status);
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
static void *_websock_create_pack(uint8_t fin, uint8_t prot, char key[4], void *data, size_t dlens, size_t *size) {
    *size = _websock_create_callens(key, dlens);
    char *frame;
    MALLOC(frame, *size);
    frame[0] = 0;
    frame[1] = 0;
    if (0 != fin) {
        BIT_SET(frame[0], 0x80);
    }
    BIT_SET(frame[0], (prot & 0xf));
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
void *websock_pack_ping(int32_t mask, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(1, WS_PING, NULL, NULL, 0, size);
    } else {
        return _websock_create_pack(1, WS_PING, _mask_key, NULL, 0, size);
    }
}
void *websock_pack_pong(int32_t mask, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(1, WS_PONG, NULL, NULL, 0, size);
    } else {
        return _websock_create_pack(1, WS_PONG, _mask_key, NULL, 0, size);
    }
}
void *websock_pack_close(int32_t mask, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(1, WS_CLOSE, NULL, NULL, 0, size);
    } else {
        return _websock_create_pack(1, WS_CLOSE, _mask_key, NULL, 0, size);
    }
}
void *websock_pack_text(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(fin, WS_TEXT, NULL, data, dlens, size);
    } else {
        return _websock_create_pack(fin, WS_TEXT, _mask_key, data, dlens, size);
    }
}
void *websock_pack_binary(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(fin, WS_BINARY, NULL, data, dlens, size);
    } else {
        return _websock_create_pack(fin, WS_BINARY, _mask_key, data, dlens, size);
    }
}
void *websock_pack_continua(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(fin, WS_CONTINUE, NULL, data, dlens, size);
    } else {
        return _websock_create_pack(fin, WS_CONTINUE, _mask_key, data, dlens, size);
    }
}
int32_t websock_fin(websock_pack_ctx *pack) {
    return pack->fin;
}
int32_t websock_prot(websock_pack_ctx *pack) {
    return pack->prot;
}
int32_t websock_secprot(websock_pack_ctx *pack) {
    return pack->secprot;
}
void *websock_secpack(struct websock_pack_ctx *pack) {
    return pack->secpack;
}
char *websock_data(websock_pack_ctx *pack, size_t *lens) {
    *lens = pack->dlens;
    return pack->data;
}
char *websock_pack_handshake(const char *host, const char *secprot) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_req(&bwriter, "GET", "/");
    if (!EMPTYSTR(host)) {
        http_pack_head(&bwriter, "Host", host);
    }
    http_pack_head(&bwriter, "Upgrade", "websocket");
    http_pack_head(&bwriter, "Connection", "Upgrade,Keep-Alive");
    http_pack_head(&bwriter, "Sec-WebSocket-Key", _hs_key);
    http_pack_head(&bwriter, "Sec-WebSocket-Version", "13");
    if (!EMPTYSTR(secprot)) {
        http_pack_head(&bwriter, "Sec-WebSocket-Protocol", secprot);
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
