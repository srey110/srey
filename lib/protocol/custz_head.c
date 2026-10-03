#include "protocol/custz_head.h"
#include "protocol/prots_pub.h"
#include "utils/utils.h"

#define CUSTZ_FIXED_LENS 4 // 固定头长度（字节数）

// 固定 4 字节头解码：缓冲区不足则等待更多数据，读取大端 4 字节整数为数据体长度
int32_t _custz_decode_fixed(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status) {
    if (buffer_size(buf) < CUSTZ_FIXED_LENS) {
        BIT_SET(*status, PROT_MOREDATA);
        return ERR_FAILED;
    }
    *hlens = CUSTZ_FIXED_LENS;
    char head[CUSTZ_FIXED_LENS];
    ASSERTAB(sizeof(head) == buffer_copyout(buf, 0, head, sizeof(head)), "copy buffer error.");
    *size = (size_t)read_be32(head);
    return ERR_OK;
}
// 固定 4 字节头编码：分配连续内存，将数据长度写入头部前 4 字节；
// 上限 UINT32_MAX，超出返回 NULL（同 _custz_encode_variable 的 268435455 上限）
char *_custz_encode_fixed(size_t dlens, size_t *hlens, size_t *size) {
    *hlens = CUSTZ_FIXED_LENS;
    // 上界与加法回绕各挡一次，判据与转 uint64_t 的理由同 _custz_encode_flag
    if ((uint64_t)dlens > UINT32_MAX
        || dlens > SIZE_MAX - *hlens) {
        *size = 0;
        return NULL;
    }
    *size = *hlens + dlens;
    char *pack;
    MALLOC(pack, *size);
    write_be32(pack, (uint32_t)dlens);
    return pack;
}
// 标志位变长头解码：首字节 <=0xfc 表示长度即为该值，0xfd/0xfe/0xff 分别后跟 2/4/8 字节长度
int32_t _custz_decode_flag(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status) {
    if (buffer_size(buf) < 1) {
        BIT_SET(*status, PROT_MOREDATA);
        return ERR_FAILED;
    }
    uint8_t flag = buffer_at(buf, 0);
    *hlens = sizeof(flag);
    if (flag <= 0xfc) {
        // 单字节编码，长度即为 flag 值
        *size = flag;
    } else if (0xfd == flag) {
        // 后跟 2 字节长度
        char buf16[sizeof(uint16_t)];
        *hlens += sizeof(buf16);
        if (*hlens > buffer_size(buf)) {
            BIT_SET(*status, PROT_MOREDATA);
            return ERR_FAILED;
        }
        ASSERTAB(sizeof(buf16) == buffer_copyout(buf, sizeof(flag), buf16, sizeof(buf16)), "copy buffer error.");
        *size = (size_t)read_be16(buf16);
    } else if (0xfe == flag) {
        // 后跟 4 字节长度
        char buf32[sizeof(uint32_t)];
        *hlens += sizeof(buf32);
        if (*hlens > buffer_size(buf)) {
            BIT_SET(*status, PROT_MOREDATA);
            return ERR_FAILED;
        }
        ASSERTAB(sizeof(buf32) == buffer_copyout(buf, sizeof(flag), buf32, sizeof(buf32)), "copy buffer error.");
        *size = (size_t)read_be32(buf32);
    } else {
        // 0xff：后跟 8 字节长度
        char buf64[sizeof(uint64_t)];
        *hlens += sizeof(buf64);
        if (*hlens > buffer_size(buf)) {
            BIT_SET(*status, PROT_MOREDATA);
            return ERR_FAILED;
        }
        ASSERTAB(sizeof(buf64) == buffer_copyout(buf, sizeof(flag), buf64, sizeof(buf64)), "copy buffer error.");
        uint64_t val64 = read_be64(buf64);
#if SIZE_MAX < UINT64_MAX// 仅在 32 位平台
        if (val64 > (uint64_t)SIZE_MAX) {
            BIT_SET(*status, PROT_ERROR);
            return ERR_FAILED;
        }
#endif
        *size = (size_t)val64;
    }
    return ERR_OK;
}
// 标志位变长头编码：按数据长度范围自动选择 1/3/5/9 字节头部格式
char *_custz_encode_flag(size_t dlens, size_t *hlens, size_t *size) {
    char *pack;
    *hlens = sizeof(uint8_t);
    if (dlens <= 0xfc) {
        // 单字节头：长度直接作为标志字节
        *size = *hlens + dlens;
        MALLOC(pack, *size);
        pack[0] = (uint8_t)dlens;
    } else if (dlens <= USHRT_MAX) {
        // 3 字节头：标志 0xfd + 2 字节长度
        *hlens += sizeof(uint16_t);
        *size = *hlens + dlens;
        MALLOC(pack, *size);
        pack[0] = 0xfd;
        write_be16(pack + sizeof(uint8_t), (uint16_t)dlens);
    } else if ((uint64_t)dlens <= UINT32_MAX) {
        // 5 字节头：标志 0xfe + 4 字节长度。
        // 上界先转 uint64_t 再比，size_t 为 32 位的构建上直接比是恒真，-Wtype-limits 会报错
        *hlens += sizeof(uint32_t);
        if (dlens > SIZE_MAX - *hlens) {
            *size = 0;
            return NULL;
        }
        *size = *hlens + dlens;
        MALLOC(pack, *size);
        pack[0] = 0xfe;
        write_be32(pack + sizeof(uint8_t), (uint32_t)dlens);
    } else {
        // 9 字节头：标志 0xff + 8 字节长度
        *hlens += sizeof(uint64_t);
        if (dlens > SIZE_MAX - *hlens) {
            *size = 0;
            return NULL;
        }
        *size = *hlens + dlens;
        MALLOC(pack, *size);
        pack[0] = 0xff;
        write_be64(pack + sizeof(uint8_t), (uint64_t)dlens);
    }
    return pack;
}
// MQTT 风格变长头解码：调用 varint_decode_mqtt 取 7-bit 内核；缓冲不足/越限按 status 协议分流
int32_t _custz_decode_variable(buffer_ctx *buf, size_t *hlens, size_t *size, int32_t *status) {
    size_t blens = buffer_size(buf);
    if (blens < 1) {
        BIT_SET(*status, PROT_MOREDATA);
        return ERR_FAILED;
    }
    int32_t occupy = varint_decode_mqtt(buf, 0, blens, size);
    if (occupy > 0) {
        *hlens = (size_t)occupy;
        return ERR_OK;
    }
    // 缓冲区已有 4 字节但仍未结束，视为协议错误；否则等待更多数据
    if (blens >= 4) {
        BIT_SET(*status, PROT_ERROR);
    } else {
        BIT_SET(*status, PROT_MOREDATA);
    }
    return ERR_FAILED;
}
// MQTT 风格变长头编码：调用 varint_encode_mqtt 取 7-bit 内核；超 MQTT_VARINT_MAX 返回 NULL
char *_custz_encode_variable(size_t dlens, size_t *hlens, size_t *size) {
    if (dlens > MQTT_VARINT_MAX) {
        *size = 0;
        return NULL;
    }
    char *pack;
    MALLOC(pack, 4 + dlens); // 头部最多 4 字节（与解码器对齐）
    int32_t n = varint_encode_mqtt((uint32_t)dlens, pack);
    *hlens = (size_t)n;
    *size = dlens + *hlens;
    return pack;
}
