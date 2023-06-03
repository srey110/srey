#include "proto/websock.h"
#include "proto/protocb.h"
#include "proto/http.h"
#include "netaddr.h"
#include "netutils.h"
#include "loger.h"

#define SIGNKEY_LENS 8
#define PLLENS_125 125
#define PLLENS_126 126
#define PLLENS_127 127
#define HEAD_BASE_LEN 6
#define HEAD_EXT16_LEN 8
#define HEAD_EXT64_LEN 14
#define SIGNKEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define RSP_OK " 101 Switching Protocols"

typedef enum parse_status {
    INIT = 0,
    START,
    HEAD,
    LENGTH,
    MASK,
    DATA
}parse_status;
typedef enum  websock_proto {
    CONTINUE = 0x00,
    TEXT = 0x01,
    BINARY = 0x02,
    CLOSE = 0x08,
    PING = 0x09,
    PONG = 0x0A
}websock_proto;
typedef struct websock_pack_ctx {
    char fin;
    char rsv1;
    char rsv2;
    char rsv3;
    char mask;
    uint16_t proto;
    unsigned char pllen;
}websock_pack_ctx;

static inline int32_t _check_keyval(http_header_ctx *head, const char *key, const char *val) {
    size_t lens = strlen(key);
    if (head->klen >= lens
        && 0 == _memicmp(head->key, key, lens)) {
        if (NULL != val) {
            lens = strlen(val);
            if (head->vlen >= lens
                && 0 == _memicmp(head->value, val, lens)) {
                return ERR_OK;
            }
        } else {
            return ERR_OK;
        }
    }
    return ERR_FAILED;
}
static inline http_header_ctx *_websock_handshake_clientcheck(void *pack) {
    size_t flens;
    size_t oklens = strlen(RSP_OK);
    const char *fdata = http_method(pack, &flens);
    if (NULL == fdata
        || flens < oklens) {
        return NULL;
    }
    const char *pos = (const char *)memchr(fdata, ' ', flens);
    if (NULL == pos
        || flens - (pos - fdata) < oklens
        || 0 != _memicmp(pos, RSP_OK, oklens)) {
        return NULL;
    }
    http_header_ctx *head;
    http_header_ctx *sign = NULL;
    uint8_t conn = 0, upgrade = 0;
    size_t cnt = http_nheader(pack);
    for (size_t i = 0; i < cnt; i++) {
        head = http_header_at(pack, i);
        switch (tolower(*head->key)) {
        case 'c':
            if (0 == conn
                && ERR_OK == _check_keyval(head, "connection", "upgrade")) {
                conn = 1;
            }
            break;
        case 'u':
            if (0 == upgrade
                && ERR_OK == _check_keyval(head, "upgrade", "websocket")) {
                upgrade = 1;
            }
            break;
        case 's':
            if (NULL == sign
                && ERR_OK == _check_keyval(head, "sec-websocket-accept", NULL)) {
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
static inline void _websock_handshake_client(SOCKET fd, void *pack, ud_cxt *ud, int32_t *closefd) {
    http_header_ctx *signstr = _websock_handshake_clientcheck(pack);
    if (NULL == signstr) {
        *closefd = 1;
        LOG_WARN("handshake failed, param check error.");
        return;
    }
    char *key;
    size_t klens = strlen(SIGNKEY);
    size_t rlens = strlen((const char *)ud->extra);
    size_t lens = klens + rlens;
    MALLOC(key, lens);
    memcpy(key, ud->extra, rlens);
    memcpy(key + rlens, SIGNKEY, klens);
    FREE(ud->extra);
    char sha1str[20];
    sha1(key, lens, sha1str);
    FREE(key);
    key = b64encode(sha1str, sizeof(sha1str), &lens);
    if (lens != signstr->vlen
        || 0 != memcmp(key, signstr->value, signstr->vlen)) {
        FREE(key);
        *closefd = 1;
        LOG_WARN("handshake failed, sign check error.");
        return;
    }
    FREE(key);
    ud->status = START;
    ((push_connmsg)ud->hscb)(fd, ERR_OK, ud);
}
static inline http_header_ctx *_websock_handshake_svcheck(void *pack) {
    size_t flens;
    size_t glens = strlen("get");
    const char *fdata = http_method(pack, &flens);
    if (NULL == fdata
        || glens > flens) {
        return NULL;
    }
    if (0 != _memicmp(fdata, "get", glens)) {
        return NULL;
    }
    http_header_ctx *head;
    http_header_ctx *sign = NULL;
    uint8_t conn = 0, upgrade = 0, version = 0, checked = 0;
    size_t cnt = http_nheader(pack);
    for (size_t i = 0; i < cnt; i++) {
        head = http_header_at(pack, i);
        switch (tolower(*head->key)) {
        case 'c':
            if (0 == conn
                && ERR_OK == _check_keyval(head, "connection", "upgrade")) {
                conn = 1;
            }
            break;
        case 'u':
            if (0 == upgrade
                && ERR_OK == _check_keyval(head, "upgrade", "websocket")) {
                upgrade = 1;
            }
            break;
        case 's':
            checked = 0;
            if (0 == version
                && ERR_OK == _check_keyval(head, "sec-websocket-version", "13")) {
                version = 1;
                checked = 1;
            }
            if (0 == checked
                && NULL == sign
                && ERR_OK == _check_keyval(head, "sec-websocket-key", NULL)) {
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
static inline void _websock_handshake_server(ev_ctx *ev, SOCKET fd, void *pack, ud_cxt *ud, int32_t *closefd) {
    http_header_ctx *signstr = _websock_handshake_svcheck(pack);
    if (NULL == signstr) {
        *closefd = 1;
        LOG_WARN("handshake failed, param check error.");
        return;
    }
    char *key;
    size_t klens = strlen(SIGNKEY);
    size_t lens = klens + signstr->vlen;
    MALLOC(key, lens);
    memcpy(key, signstr->value, signstr->vlen);
    memcpy(key + signstr->vlen, SIGNKEY, klens);
    char sha1str[20];
    sha1(key, lens, sha1str);
    FREE(key);
    key = b64encode(sha1str, sizeof(sha1str), &lens);
    static const char *fmt = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n";
    char *rsp = formatv(fmt, key);
    FREE(key);
    ud->status = START;
    ((push_acptmsg)ud->hscb)(fd, ud);
    ev_send(ev, fd, rsp, strlen(rsp), 0);
}
static inline void _websock_handshake(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, ud_cxt *ud, int32_t *closefd) {
    int32_t status;
    void *pack = _http_parsehead(buf, &status, closefd);
    if (NULL == pack) {
        return;
    }
    if (0 != status) {
        *closefd = 1;
        http_pkfree(pack);
        LOG_WARN("handshake failed, status error %d.", status);
        return;
    }
    if (0 != ud->svside) {
        _websock_handshake_server(ev, fd, pack, ud, closefd);
    } else {
        _websock_handshake_client(fd, pack, ud, closefd);
    }
    http_pkfree(pack);
}
void *websock_unpack(ev_ctx *ev, SOCKET fd, buffer_ctx *buf, size_t *size, ud_cxt *ud, int32_t *closefd) {
    void *data = NULL;
    switch (ud->status) {
    case INIT:
        _websock_handshake(ev, fd, buf, ud, closefd);
        break;
    default:
        break;
    }
    return data;
}
void *websock_pack(void *data, size_t lens, size_t *size) {
    return NULL;
}
int32_t websock_client_reqhs(ev_ctx *ev, SOCKET fd, ud_cxt *ud) {
    char rdstr[SIGNKEY_LENS + 1];
    randstr(rdstr, sizeof(rdstr) - 1);
    size_t blens;
    char *b64 = b64encode(rdstr, strlen(rdstr), &blens);
    char *data;
    const char *fmt = "GET / HTTP/1.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n";
    data = formatv(fmt, b64);
    ev_send(ev, fd, data, strlen(data), 0);
    ud->extra = b64;
    return ERR_OK;
}
