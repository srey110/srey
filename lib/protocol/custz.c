#include "protocol/custz.h"
#include "protocol/custz_head.h"

void *custz_unpack(struct ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    (void)ev; (void)sk; (void)client;
    size_t hlens;
    int32_t rtn = ERR_FAILED;
    // 根据包类型调用对应的头部解码函数；子类型取自 ud，与另外九个 *_unpack 同签名
    switch ((pack_type)ud->pktype) {
    case PACK_CUSTZ_FIXED:
        rtn = _custz_decode_fixed(buf, &hlens, size, status);
        break;
    case PACK_CUSTZ_FLAG:
        rtn = _custz_decode_flag(buf, &hlens, size, status);
        break;
    case PACK_CUSTZ_VAR:
        rtn = _custz_decode_variable(buf, &hlens, size, status);
        break;
    default:
        ASSERTAB(0, "unknow pack type");
        break;
    }
    if (ERR_OK != rtn) {
        return NULL;
    }
    // 零长包组包侧不组，收到与超限一样按协议错误断开
    if (0 == *size
        || *size > CUSTZ_MAX_PACK_LENS) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    size_t pklens = hlens + *size;
    // 缓冲区数据不足，等待更多数据
    if (pklens > buffer_size(buf)) {
        BIT_SET(*status, PROT_MOREDATA);
        buffer_want(buf, pklens - buffer_size(buf));
        return NULL;
    }
    char *msg;
    MALLOC(msg, *size);
    // 从缓冲区复制数据体（跳过头部）
    ASSERTAB(*size == buffer_copyout(buf, hlens, msg, *size), "copy buffer error.");
    // 消耗整个包（头部 + 数据体）
    ASSERTAB(pklens == buffer_drain(buf, pklens), "drain buffer error.");
    return msg;
}
void *custz_pack(pack_type pktype, void *data, size_t lens, size_t *size) {
    if (0 == lens) {
        *size = 0;
        return NULL;
    }
    size_t hlens;
    char *pack = NULL;
    // 根据包类型调用对应的头部编码函数，分配头部+数据体的连续内存
    switch (pktype) {
    case PACK_CUSTZ_FIXED:
        pack = _custz_encode_fixed(lens, &hlens, size);
        break;
    case PACK_CUSTZ_FLAG:
        pack = _custz_encode_flag(lens, &hlens, size);
        break;
    case PACK_CUSTZ_VAR:
        pack = _custz_encode_variable(lens, &hlens, size);
        break;
    default:
        ASSERTAB(0, "unknow pack type");
        break;
    }
    if (NULL == pack) {
        return NULL;
    }
    memcpy(pack + hlens, data, lens);// 将数据体拷贝到头部之后的位置
    return pack;
}
