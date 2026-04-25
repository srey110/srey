#include "protocol/custz.h"
#include "protocol/custz_head.h"
#include "protocol/prots.h"

void *custz_unpack(uint8_t pktype, buffer_ctx *buf, size_t *size, int32_t *status) {
    size_t hlens;
    int32_t rtn;
    // 根据包类型调用对应的头部解码函数
    switch (pktype) {
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
    // 数据体长度超限，协议错误
    if (PACK_TOO_LONG(*size)) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    size_t pklens = hlens + *size;
    // 缓冲区数据不足，等待更多数据
    if (pklens > buffer_size(buf)) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    // 数据体为空，仅消耗头部后返回 NULL
    if (0 == *size) {
        ASSERTAB(hlens == buffer_drain(buf, hlens), "drain buffer error.");
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
void *custz_pack(uint8_t pktype, void *data, size_t lens, size_t *size) {
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
    // 将数据体拷贝到头部之后的位置
    if (0 != lens) {
        memcpy(pack + hlens, data, lens);
    }
    return pack;
}
