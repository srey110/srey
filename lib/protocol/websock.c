#include "protocol/websock.h"
#include "protocol/prots.h"
#include "protocol/http.h"
#include "protocol/mqtt/mqtt.h"
#include "event/event.h"
#include "utils/utils.h"

#define MASK_KEY_LENS  4  //掩码密钥长度
#define SIGN_KEY_LENS  16 //握手签名（RFC 6455：16 字节随机 nonce，base64 后 24 字符）
#define HEAD_LESN      2  // WebSocket 帧最小头部长度（字节）
#define SIGNKEY "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" // WebSocket 握手固定密钥后缀（RFC 6455）

// WebSocket 帧解析状态
typedef enum parse_status {
    INIT = 0,  // 初始状态，等待 HTTP 握手
    START,     // 握手完成，等待 WebSocket 帧头
    DATA       // 已解析帧头，等待帧数据
}parse_status;
// WebSocket 数据包上下文
typedef struct websock_pack_ctx {
    int8_t fin;          // FIN 位：1=完整帧或最后一帧，0=分片中间帧
    int8_t prot;         // 操作码（ws_prot 枚举值）
    int8_t mask;         // 是否使用掩码（客户端发送时为 1）
    pack_type secprot;   // 子协议类型（如 PACK_MQTT）
    size_t remain;       // 帧数据段在缓冲区中的剩余待读字节数
    size_t dlens;        // 数据体长度（不含掩码键）
    void *secpack;       // 子协议解包结果
    char key[MASK_KEY_LENS]; // 掩码密钥（mask=1 时有效）
    char data[];         // 数据体（柔性数组）
}websock_pack_ctx;
// WebSocket 连接上下文（每个连接持有一个）
typedef struct websock_ctx {
    int8_t slice;          // 是否处于分片接收状态（1=是）
    pack_type secprot;     // 子协议类型
    buffer_ctx *buf;       // 子协议数据缓冲区
    ud_cxt *ud;            // 子协议的 ud_cxt（用于子协议解包）
    websock_pack_ctx *pack; // 当前正在解析的帧（DATA 状态下有效）
}websock_ctx;

static _handshaked_push _hs_push;                      // 握手完成后的推送回调

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #include <emmintrin.h>
    #define WEBSOCK_MASK_HAS_SSE2 1
#endif
#if defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define WEBSOCK_MASK_HAS_NEON 1
#endif
// 用 4 字节掩码对 data 做 XOR，按平台选择 SIMD 路径；尾部 < 块大小 字节走 8 字节标量
static inline void _websock_mask_xor(char *data, size_t lens, const char key[4]) {
    size_t i = 0;
#if defined(WEBSOCK_MASK_HAS_SSE2)
    uint32_t key32;
    __m128i vkey, v;
    memcpy(&key32, key, 4);
    vkey = _mm_set1_epi32((int32_t)key32);
    for (; i + 16 <= lens; i += 16) {
        v = _mm_loadu_si128((const __m128i *)(data + i));
        v = _mm_xor_si128(v, vkey);
        _mm_storeu_si128((__m128i *)(data + i), v);
    }
#elif defined(WEBSOCK_MASK_HAS_NEON)
    // 用 u8 load/store + reinterpret 确保 ISO 严格别名合规（字节类型可访问任意内存）；
    // vreinterpretq 是位级类型重解释，零运行时开销
    uint32_t key32;
    uint8x16_t vkey, v;
    memcpy(&key32, key, 4);
    vkey = vreinterpretq_u8_u32(vdupq_n_u32(key32));
    for (; i + 16 <= lens; i += 16) {
        v = vld1q_u8((const uint8_t *)(data + i));
        v = veorq_u8(v, vkey);
        vst1q_u8((uint8_t *)(data + i), v);
    }
#endif
    // 8 字节标量块（覆盖 SIMD 尾部 16 字节内的 8 字节对齐残余，或无 SIMD 平台主路径）
    uint32_t key32s;
    uint64_t key64, block;
    memcpy(&key32s, key, 4);
    key64 = (uint64_t)key32s | ((uint64_t)key32s << 32);
    for (; i + 8 <= lens; i += 8) {
        memcpy(&block, data + i, 8);
        block ^= key64;
        memcpy(data + i, &block, 8);
    }
    // 最后 0-7 字节
    for (; i < lens; i++) {
        data[i] ^= key[i & 3];
    }
}

void _websock_pkfree(void *data) {
    if (NULL == data) {
        return;
    }
    websock_pack_ctx *pack = (websock_pack_ctx *)data;
    prots_pkfree(pack->secprot, pack->secpack);
    FREE(data);
}
void _websock_udfree(ud_cxt *ud) {
    if (NULL == ud->context) {
        return;
    }
    //客户端还未完成握手，释放signkey
    if (INIT == ud->status) {
        FREE(ud->context);
        return;
    }
    websock_ctx *ws = (websock_ctx *)ud->context;
    _websock_pkfree(ws->pack);
    prots_udfree(ws->ud);
    FREE(ws->ud);
    if (NULL != ws->buf) {
        buffer_free(ws->buf);
        FREE(ws->buf);
    }
    FREE(ws);
    ud->context = NULL;
}
void _websock_secextra(ud_cxt *ud, void *val) {
    if (NULL == ud->context) {
        LOG_WARN("set second ud_cxt extra data error.");
        return;
    }
    websock_ctx *ws = (websock_ctx *)ud->context;
    if (NULL == ws->ud) {
        LOG_WARN("set second ud_cxt extra data error.");
        return;
    }
    ws->ud->context = val;
}
/* 直接用 HTTP 解析器返回的原始指针（非 NUL 结尾）比对已知子协议名称，
 * 长度和内容均须匹配，无需额外内存分配。 */
static int32_t _websock_sec_prot(const char *data, size_t lens, pack_type *sectype) {
    if (lens == sizeof("mqtt") - 1
        && 0 == _memicmp(data, "mqtt", sizeof("mqtt") - 1)) {
        *sectype = PACK_MQTT;
        return ERR_OK;
    }
    return ERR_FAILED;
}
// 服务端侧握手校验：验证 GET 请求中的 Connection/Upgrade/Sec-WebSocket-Version/Key 字段
static http_header_ctx *_websock_handshake_svcheck(struct http_pack_ctx *hpack) {
    buf_ctx *status = http_status(hpack);
    if (!buf_icompare(&status[0], "get", sizeof("get") - 1)) {
        return NULL;
    }
    http_header_ctx *head;
    http_header_ctx *sign = NULL;
    uint8_t conn = 0, upgrade = 0, version = 0;
    uint32_t cnt = http_nheader(hpack);
    for (uint32_t i = 0; i < cnt; i++) {
        head = http_header_at(hpack, i);
        /* buf_icompare 先做长度比较（O(1)），各键长度均不同，无需首字符 switch。 */
        if (0 == conn
            && ERR_OK == _http_check_keyval(head,
                                            "connection", sizeof("connection") - 1,
                                            "upgrade",   sizeof("upgrade") - 1)) {
            conn = 1;
        }
        if (0 == upgrade
            && ERR_OK == _http_check_keyval(head,
                                            "upgrade",   sizeof("upgrade") - 1,
                                            "websocket", sizeof("websocket") - 1)) {
            upgrade = 1;
        }
        if (0 == version
            && ERR_OK == _http_check_keyval(head,
                                            "sec-websocket-version", sizeof("sec-websocket-version") - 1,
                                            "13",                    sizeof("13") - 1)) {
            version = 1;
        }
        if (NULL == sign
            && ERR_OK == _http_check_keyval(head,
                                            "sec-websocket-key", sizeof("sec-websocket-key") - 1,
                                            NULL, 0)) {
            sign = head;
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
        || 0 == version
        || NULL == sign) {
        return NULL;
    }
    return sign;
}
// 计算 WebSocket 握手签名 SHA1 哈希再 base64 编码
static void _websock_sign(char *key, size_t klens, char bs64sha1[B64EN_SIZE(SHA1_BLOCK_SIZE)]) {
    char sha1str[SHA1_BLOCK_SIZE];
    digest_ctx digest;
    digest_init(&digest, DG_SHA1);
    digest_update(&digest, key, klens);
    digest_update(&digest, SIGNKEY, sizeof(SIGNKEY) - 1);
    digest_final(&digest, sha1str);
    bs64_encode(sha1str, sizeof(sha1str), bs64sha1);
}
// 服务端握手处理：发送 101 响应并通知上层握手成功（或失败）
static int32_t _websock_handshake_server(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    ud_cxt *ud, struct http_pack_ctx *hpack, int32_t *status, pack_type *sectype) {
    http_header_ctx *signstr = _websock_handshake_svcheck(hpack);
    if (NULL == signstr
        || 0 == signstr->value.lens
        || signstr->value.lens > B64EN_SIZE(SIGN_KEY_LENS)) {
        BIT_SET(*status, PROT_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return ERR_FAILED;
    }
    //签名base64校验
    //缓冲区按 B64DE_SIZE(B64EN_SIZE(SIGN_KEY_LENS))=22 字节分配，避免恶意客户端传入
    //合法长度区间内（lens<25）但解码字节数 >SIGN_KEY_LENS 时 bs64_decode 在校验返回值前写越界
    char key[B64DE_SIZE(B64EN_SIZE(SIGN_KEY_LENS))];
    if (SIGN_KEY_LENS != bs64_decode(signstr->value.data, signstr->value.lens, key)) {
        BIT_SET(*status, PROT_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return ERR_FAILED;
    }
    //子协议校验
    size_t lens = 0;
    char *sechead = http_header(hpack, "Sec-WebSocket-Protocol", &lens);
    char *secprot = NULL;
    if (NULL != sechead && 0 != lens) {
        /* 先直接比较原始头部值（非 NUL 结尾指针），
         * 仅在协议被支持时才分配持久化副本。 */
        if (ERR_OK != _websock_sec_prot(sechead, lens, sectype)) {
            BIT_SET(*status, PROT_ERROR);
            _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
            return ERR_FAILED;
        }
        MALLOC(secprot, lens + 1);
        memcpy(secprot, sechead, lens);
        secprot[lens] = '\0';
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
    if (ERR_OK != ev_send(ev, fd, skid, bwriter.data, bwriter.offset, 0)) {
        BIT_SET(*status, PROT_ERROR);
        //ev_send(copy=0) 失败时 bwriter.data 由事件层接管释放；
        //secprot 已分配但未交付 _hs_push，必须本函数释放避免泄漏
        FREE(secprot);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return ERR_FAILED;
    }
    //secprot 已随握手消息交付 _hs_push，最终在 _message_clean 中释放
    if (ERR_OK != _hs_push(fd, skid, client, ud, ERR_OK, secprot, lens)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    } else {
        ud->status = START;
        return ERR_OK;
    }
}
// 客户端侧握手状态行校验：确认响应状态码为 101
static int32_t _websock_handshake_clientckstatus(struct http_pack_ctx *hpack) {
    buf_ctx *status = http_status(hpack);
    if (!buf_compare(&status[1], "101", strlen("101"))) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 客户端侧握手头部校验：验证 Connection/Upgrade/Sec-WebSocket-Accept 字段
static http_header_ctx *_websock_client_checkhs(struct http_pack_ctx *hpack) {
    if (ERR_OK != _websock_handshake_clientckstatus(hpack)) {
        return NULL;
    }
    http_header_ctx *head;
    http_header_ctx *sign = NULL;
    uint8_t conn = 0, upgrade = 0;
    uint32_t cnt = http_nheader(hpack);
    for (uint32_t i = 0; i < cnt; i++) {
        head = http_header_at(hpack, i);
        if (0 == conn
            && ERR_OK == _http_check_keyval(head,
                                            "connection", sizeof("connection") - 1,
                                            "upgrade",   sizeof("upgrade") - 1)) {
            conn = 1;
        }
        if (0 == upgrade
            && ERR_OK == _http_check_keyval(head,
                                            "upgrade",   sizeof("upgrade") - 1,
                                            "websocket", sizeof("websocket") - 1)) {
            upgrade = 1;
        }
        if (NULL == sign
            && ERR_OK == _http_check_keyval(head,
                                            "sec-websocket-accept", sizeof("sec-websocket-accept") - 1,
                                            NULL, 0)) {
            sign = head;
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
// 客户端握手处理：验证服务端响应的 Accept 签名并通知上层握手成功（或失败）
static int32_t _websock_handshake_client(SOCKET fd, uint64_t skid, int32_t client, ud_cxt *ud,
    struct http_pack_ctx *hpack, int32_t *status, pack_type *sectype) {
    char *signkey = (char *)ud->context;
    if (NULL == signkey) {
        BIT_SET(*status, PROT_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return ERR_FAILED;
    }
    //签名校验
    http_header_ctx *signstr = _websock_client_checkhs(hpack);
    if (NULL == signstr
        || !buf_compare(&signstr->value, signkey, strlen(signkey))) {
        BIT_SET(*status, PROT_ERROR);
        _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
        return ERR_FAILED;
    }
    //子协议校验
    size_t lens = 0;
    char *sechead = http_header(hpack, "Sec-WebSocket-Protocol", &lens);
    char *secprot = NULL;
    if (NULL != sechead && 0 != lens) {
        if (ERR_OK != _websock_sec_prot(sechead, lens, sectype)) {
            BIT_SET(*status, PROT_ERROR);
            _hs_push(fd, skid, client, ud, ERR_FAILED, NULL, 0);
            return ERR_FAILED;
        }
        MALLOC(secprot, lens + 1);
        memcpy(secprot, sechead, lens);
        secprot[lens] = '\0';
    }
    //secprot 最终在 _message_clean 释放
    if (ERR_OK != _hs_push(fd, skid, client, ud, ERR_OK, secprot, lens)) {
        BIT_SET(*status, PROT_ERROR);
        return ERR_FAILED;
    } else {
        ud->status = START;
        return ERR_OK;
    }
}
// WebSocket 握手入口：解析 HTTP 头部后根据 client 标志分发到服务端或客户端握手处理
static void _websock_handshake(ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t transfer;
    struct http_pack_ctx *hpack = _http_parsehead(buf, ud, &transfer, status);
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
        if (client) {
            //客户端释放signkey
            FREE(ud->context);
        }
        CALLOC(ud->context, 1, sizeof(websock_ctx));
        websock_ctx *ws = (websock_ctx *)ud->context;
        ws->secprot = sectype;
        if (PACK_NONE != sectype) {//加密协议
            MALLOC(ws->buf, sizeof(buffer_ctx));
            buffer_init(ws->buf);
            CALLOC(ws->ud, 1, sizeof(ud_cxt));
            ws->ud->pktype = sectype;
            ws->ud->handle = ud->handle;
            ws->ud->loader = ud->loader;
        }
    }
    _http_pkfree(hpack);
}
// 释放以 websock_pack_ctx 为容器的 MQTT 数据包（通过 UPCAST 找到结构体头部）
static void _websock_mqtt_buffree(void *buf) {
    websock_pack_ctx *pack = UPCAST(buf, websock_pack_ctx, data);
    _websock_pkfree(pack);
}
// 将 WebSocket 帧数据交给 MQTT 子协议解包，返回包含 MQTT 包的 websock_pack_ctx
static websock_pack_ctx *_websock_sec_mqtt(websock_ctx *ws, websock_pack_ctx *pack, int32_t client, int32_t *status) {
    if (WS_BINARY != pack->prot && WS_CONTINUE != pack->prot) {
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
// 子协议统一解包入口，将 WebSocket 帧数据转发给对应子协议处理
static websock_pack_ctx *_websock_sec_unpack(websock_ctx *ws, websock_pack_ctx *pack, int32_t client, int32_t *status) {
    websock_pack_ctx *rtn = NULL;
    switch (ws->secprot) {
    case PACK_MQTT:
        rtn = _websock_sec_mqtt(ws, pack, client, status);
        break;
    default:
        _websock_pkfree(pack);
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    // 子协议解包循环由外层调用方负责，移除 MOREDATA 标志避免外层误判
    if (BIT_CHECK(*status, PROT_MOREDATA)) {
        BIT_REMOVE(*status, PROT_MOREDATA);
    }
    return rtn;
}
// 读取 WebSocket 帧数据体（含掩码解码），设置分片状态标志，按需交子协议处理
static websock_pack_ctx *_websock_parse_data(buffer_ctx *buf, int32_t client, ud_cxt *ud, int32_t *status) {
    websock_ctx *ws = (websock_ctx *)ud->context;
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
            _websock_mask_xor(pack->data, pack->dlens, pack->key);
        }
        ASSERTAB(pack->remain == buffer_drain(buf, pack->remain), "drain buffer failed.");
    }
    // 分片帧判断：起始帧 FIN=0 且 opcode≠0；中间帧 FIN=0 且 opcode=0；结束帧 FIN=1 且 opcode=0
    // 分片合法性（CONTINUE 必须在分片中、TEXT/BINARY 不可在分片中）已在 _websock_parse_head 前置拒绝
    if (0 == pack->fin && 0 != pack->prot) {
        ws->slice = 1;
        BIT_SET(*status, PROT_SLICE_START);
    } else if (0 == pack->fin && 0 == pack->prot) {
        BIT_SET(*status, PROT_SLICE);
    } else if (1 == pack->fin && 0 == pack->prot) {
        ws->slice = 0;
        BIT_SET(*status, PROT_SLICE_END);
    }
    if (WS_CLOSE == pack->prot) {
        BIT_SET(*status, PROT_CLOSE);
    }
    ws->pack = NULL;
    ud->status = START;
    if (PACK_NONE != ws->secprot // 存在子协议
        && pack->dlens > 0      // 排除空包
        && (WS_CONTINUE == pack->prot
            || WS_TEXT == pack->prot
            || WS_BINARY == pack->prot)) {
        return _websock_sec_unpack(ws, pack, client, status);
    } else {
        return pack;
    }
}
// 根据 payloadlen 字段（7位）解析真实数据长度并分配 websock_pack_ctx
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
            BIT_SET(*status, PROT_MOREDATA);
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
            BIT_SET(*status, PROT_MOREDATA);
            return NULL;
        }
        ASSERTAB(sizeof(pllens) == buffer_copyout(buf, HEAD_LESN, &pllens, sizeof(pllens)), "copy buffer failed.");
        pllens = ntohll(pllens);
        if (PACK_TOO_LONG(pllens)) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        // 防止 64-bit 长度在 32-bit 平台截断为 size_t 导致分配不足
        if (pllens > (uint64_t)SIZE_MAX) {
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
// 解析 WebSocket 帧头（FIN/RSV/opcode/MASK/payloadlen），校验 RSV 位和掩码要求
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
    // RFC 6455 §5.2：保留 opcode（0x3-0x7 数据帧保留、0xB-0xF 控制帧保留）必须拒绝
    if ((prot >= 0x3 && prot <= 0x7) || prot >= 0xB) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    websock_ctx *ws = (websock_ctx *)ud->context;
    if (prot >= 0x8) {
        // RFC 6455 §5.5：控制帧不可分片（FIN 必须为 1）且 payload ≤125
        if (0 == fin || payloadlen > 125) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
    } else if (WS_CONTINUE == prot) {
        // RFC 6455 §5.4：CONTINUE 必须出现在分片进行中
        if (0 == ws->slice) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
    } else {
        // RFC 6455 §5.4：TEXT/BINARY 不可出现在分片进行中（必须用 CONTINUE）
        if (0 != ws->slice) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
    }
    websock_pack_ctx *pack = _websock_parse_pllens(buf, blens, mask, payloadlen, status);
    if (NULL == pack) {
        return NULL;
    }
    pack->fin = fin;
    pack->prot = prot;
    pack->mask = mask;
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
// 计算 WebSocket 帧总长度（头部 + 可选扩展长度字段 + 可选掩码 + 数据体）
static size_t _websock_create_callens(char *key, size_t dlens) {
    size_t size = HEAD_LESN + dlens;
    if (dlens >= 126) {
        if (dlens > 0xffff) {
            size += sizeof(uint64_t);
        } else {
            size += sizeof(uint16_t);
        }
    }
    if (NULL != key) {
        size += MASK_KEY_LENS;
    }
    return size;
}
// 构造 WebSocket 帧：写入头部（含扩展长度和掩码），有掩码时对数据进行 XOR 加密
static void *_websock_create_pack(uint8_t fin, uint8_t prot, char *key, void *data, size_t dlens, size_t *size) {
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
        memcpy(frame + offset, key, MASK_KEY_LENS);
        offset += MASK_KEY_LENS;
        if (NULL != data) {
            memcpy(frame + offset, data, dlens);
            _websock_mask_xor(frame + offset, dlens, key);
        }
    } else {
        if (NULL != data) {
            memcpy(frame + offset, data, dlens);
        }
    }
    return frame;
}
// 按 mask 选 key=NULL(无掩码)或随机生成,后调 _websock_create_pack
static void *_websock_pack_frame(int32_t mask, uint8_t fin, uint8_t prot,
                                void *data, size_t dlens, size_t *size) {
    if (0 == mask) {
        return _websock_create_pack(fin, prot, NULL, data, dlens, size);
    }
    char key[MASK_KEY_LENS];
    csprng_rand(key, MASK_KEY_LENS);
    return _websock_create_pack(fin, prot, key, data, dlens, size);
}
void *websock_pack_ping(int32_t mask, size_t *size) {
    return _websock_pack_frame(mask, 1, WS_PING, NULL, 0, size);
}
void *websock_pack_pong(int32_t mask, size_t *size) {
    return _websock_pack_frame(mask, 1, WS_PONG, NULL, 0, size);
}
void *websock_pack_close(int32_t mask, size_t *size) {
    return _websock_pack_frame(mask, 1, WS_CLOSE, NULL, 0, size);
}
void *websock_pack_text(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size) {
    return _websock_pack_frame(mask, fin, WS_TEXT, data, dlens, size);
}
void *websock_pack_binary(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size) {
    return _websock_pack_frame(mask, fin, WS_BINARY, data, dlens, size);
}
void *websock_pack_continua(int32_t mask, int32_t fin, void *data, size_t dlens, size_t *size) {
    return _websock_pack_frame(mask, fin, WS_CONTINUE, data, dlens, size);
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
//生成握手签名
static void _websock_sign_keys(char bs64key[B64EN_SIZE(SIGN_KEY_LENS)], char bs64sha1key[B64EN_SIZE(SHA1_BLOCK_SIZE)]) {
    char key[SIGN_KEY_LENS + 1];
    randstr(key, SIGN_KEY_LENS);
    bs64_encode(key, SIGN_KEY_LENS, bs64key);
    _websock_sign(bs64key, strlen(bs64key), bs64sha1key);
}
char *websock_pack_handshake(const char *host, const char *uri, const char *secprot, char *signkey) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    http_pack_req(&bwriter, "GET", EMPTYSTR(uri) ? "/" : uri);
    if (!EMPTYSTR(host)) {
        http_pack_head(&bwriter, "Host", host);
    }
    http_pack_head(&bwriter, "Upgrade", "websocket");
    http_pack_head(&bwriter, "Connection", "Upgrade,Keep-Alive");
    char bs64key[B64EN_SIZE(SIGN_KEY_LENS)];
    _websock_sign_keys(bs64key, signkey);
    http_pack_head(&bwriter, "Sec-WebSocket-Key", bs64key);
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
}
