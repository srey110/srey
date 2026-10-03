#include "serial/seri.h"

#define SERI_MAX_COOKIE 32// 短字符串/短数组的 cookie 上限（含 31 表示长 array 转义）
#define COMBINE_TYPE(t, v)  ((uint8_t)((t) | ((v) << 3)))
// wire format 类型与 cookie 常量
typedef enum seri_type {
    SERI_TYPE_NIL = 0x00,
    SERI_TYPE_BOOLEAN,
    SERI_TYPE_NUMBER,
    SERI_TYPE_USERDATA,
    SERI_TYPE_SHORT_STRING,
    SERI_TYPE_LONG_STRING,
    SERI_TYPE_ARRAY,
}seri_type;
typedef enum seri_number_type {
    SERI_NUMBER_ZERO  = 0,
    SERI_NUMBER_BYTE  = 1,
    SERI_NUMBER_WORD  = 2,
    SERI_NUMBER_DWORD = 4,
    SERI_NUMBER_QWORD = 6,
    SERI_NUMBER_REAL  = 8,
}seri_number_type;

void seri_append_nil(binary_ctx *bw) {
    binary_set_uint8(bw, SERI_TYPE_NIL);
}
void seri_append_bool(binary_ctx *bw, int32_t b) {
    binary_set_uint8(bw, COMBINE_TYPE(SERI_TYPE_BOOLEAN, b ? 1 : 0));
}
// 一次扩够 n 字节并推进 offset，返回这 n 字节的起点
static inline char *_seri_reserve(binary_ctx *bw, size_t n) {
    size_t start = bw->offset;
    binary_set_skip(bw, n);
    return bw->data + start;
}
// 写 tag 加 n 字节小端整数
static inline void _seri_put_num(binary_ctx *bw, uint8_t tag, uint64_t v, size_t n) {
    char *p = _seri_reserve(bw, 1 + n);
    p[0] = (char)tag;
    write_integer(p + 1, v, n, 1);
}
void seri_append_int(binary_ctx *bw, int64_t v) {
    if (0 == v) {
        binary_set_uint8(bw, COMBINE_TYPE(SERI_TYPE_NUMBER, SERI_NUMBER_ZERO));
    } else if (v != (int32_t)v) {
        // 越出 int32 范围 → QWORD(i64)
        _seri_put_num(bw, COMBINE_TYPE(SERI_TYPE_NUMBER, SERI_NUMBER_QWORD), (uint64_t)v, 8);
    } else if (v < 0) {
        // 负数走 DWORD(i32)
        _seri_put_num(bw, COMBINE_TYPE(SERI_TYPE_NUMBER, SERI_NUMBER_DWORD), (uint64_t)v, 4);
    } else if (v < 0x100) {
        _seri_put_num(bw, COMBINE_TYPE(SERI_TYPE_NUMBER, SERI_NUMBER_BYTE), (uint64_t)v, 1);
    } else if (v < 0x10000) {
        _seri_put_num(bw, COMBINE_TYPE(SERI_TYPE_NUMBER, SERI_NUMBER_WORD), (uint64_t)v, 2);
    } else {
        // 0x10000 <= v <= INT32_MAX：DWORD(u32)(>INT32_MAX 已在上面走 QWORD)
        _seri_put_num(bw, COMBINE_TYPE(SERI_TYPE_NUMBER, SERI_NUMBER_DWORD), (uint64_t)v, 4);
    }
}
void seri_append_real(binary_ctx *bw, double v) {
    char *p = _seri_reserve(bw, 1 + sizeof(double));
    p[0] = (char)COMBINE_TYPE(SERI_TYPE_NUMBER, SERI_NUMBER_REAL);
    pack_double(p + 1, v, 1);
}
void seri_append_string(binary_ctx *bw, const char *s, size_t len) {
    // 仅 64 位守卫 4 字节长度前缀上界；32 位 size_t≤UINT32_MAX 恒真，略过以免 -Wtype-limits
#if SIZE_MAX > UINT32_MAX
    ASSERTAB(len <= UINT32_MAX, "seri string length exceeds 4GB limit");
#endif
    uintptr_t soff = (uintptr_t)s - (uintptr_t)bw->data;
    int32_t inner = (NULL != bw->data && soff < bw->size);
    size_t pre = len < SERI_MAX_COOKIE ? 0 : (len < 0x10000 ? 2 : 4);
    char *p = _seri_reserve(bw, 1 + pre + len);
    if (0 == pre) {
        p[0] = (char)COMBINE_TYPE(SERI_TYPE_SHORT_STRING, (uint8_t)len);
    } else {
        p[0] = (char)COMBINE_TYPE(SERI_TYPE_LONG_STRING, (uint8_t)pre);
        write_integer(p + 1, (uint64_t)len, pre, 1);
    }
    if (len > 0) {
        memmove(p + 1 + pre, inner ? bw->data + soff : s, len);
    }
}
void seri_append_userdata(binary_ctx *bw, void *ud) {
    _seri_put_num(bw, SERI_TYPE_USERDATA, (uint64_t)(uintptr_t)ud, sizeof(void *));
}
void seri_append_array_start(binary_ctx *bw, uint32_t array_n) {
    if (array_n < SERI_MAX_COOKIE - 1) {
        binary_set_uint8(bw, COMBINE_TYPE(SERI_TYPE_ARRAY, (uint8_t)array_n));
    } else {
        // cookie=31 转义：后跟一个完整 INT 写真实长度
        binary_set_uint8(bw, COMBINE_TYPE(SERI_TYPE_ARRAY, SERI_MAX_COOKIE - 1));
        seri_append_int(bw, (int64_t)array_n);
    }
}
void seri_append_array_end(binary_ctx *bw) {
    binary_set_uint8(bw, SERI_TYPE_NIL);
}
void seri_iter_init(seri_iter *it, const void *buf, size_t size) {
    it->buffer = (const char *)buf;
    it->len = size;
    it->offset = 0;
}
// 内部 helper：尝试读 n 字节，成功推进 offset 并返指针，失败返 NULL（不修改 offset）
static inline const char *_seri_rb_read(seri_iter *it, size_t n) {
    if (it->len - it->offset < n) {
        return NULL;
    }
    const char *p = it->buffer + it->offset;
    it->offset += n;
    return p;
}
// 根据 cookie 选档读 int64；越界 / cookie 非法返 -1
static int32_t _seri_read_integer(seri_iter *it, uint8_t cookie, int64_t *out) {
    const char *p;
    switch (cookie) {
    case SERI_NUMBER_ZERO:
        *out = 0;
        return 0;
    case SERI_NUMBER_BYTE:
        if (NULL == (p = _seri_rb_read(it, 1))) {
            return -1;
        }
        *out = (int64_t)(uint8_t)*p;
        return 0;
    case SERI_NUMBER_WORD:
        if (NULL == (p = _seri_rb_read(it, 2))) {
            return -1;
        }
        *out = (int64_t)read_le16(p);// u16 LE
        return 0;
    case SERI_NUMBER_DWORD:
        if (NULL == (p = _seri_rb_read(it, 4))) {
            return -1;
        }
        *out = (int64_t)(int32_t)read_le32(p);// i32 LE（保留负数符号）
        return 0;
    case SERI_NUMBER_QWORD:
        if (NULL == (p = _seri_rb_read(it, 8))) {
            return -1;
        }
        *out = (int64_t)read_le64(p);// i64 LE
        return 0;
    default:
        return -1;
    }
}
int32_t seri_iter_next(seri_iter *it, seri_item *out) {
    const char *p = _seri_rb_read(it, 1);
    if (NULL == p) {
        return 0;
    }
    uint8_t tag = (uint8_t)*p;
    uint8_t type = tag & 0x7;
    uint8_t cookie = tag >> 3;
    switch (type) {
    case SERI_TYPE_NIL:
        // 既用作"显式 nil 值"也用作"array hash 段结束"，由调用方按上下文区分
        out->type = SERI_ITEM_NIL;
        return 1;
    case SERI_TYPE_BOOLEAN:
        out->type = SERI_ITEM_BOOL;
        out->v.b = (0 != cookie) ? 1 : 0;
        return 1;
    case SERI_TYPE_NUMBER:
        if (SERI_NUMBER_REAL == cookie) {
            if (NULL == (p = _seri_rb_read(it, 8))) {
                return -1;
            }
            out->type = SERI_ITEM_REAL;
            out->v.r = unpack_double(p, 1);
            return 1;
        }
        out->type = SERI_ITEM_INT;
        if (0 != _seri_read_integer(it, cookie, &out->v.i)) {
            return -1;
        }
        return 1;
    case SERI_TYPE_USERDATA: {
        if (NULL == (p = _seri_rb_read(it, sizeof(void *)))) {
            return -1;
        }
        uint64_t addr = (uint64_t)read_integer(p, sizeof(void *), 1, 0);
        out->type = SERI_ITEM_USERDATA;
        out->v.ud = (void *)(uintptr_t)addr;
        return 1;
    }
    case SERI_TYPE_SHORT_STRING:
        if (cookie > 0) {
            if (NULL == (p = _seri_rb_read(it, cookie))) {
                return -1;
            }
        } else {
            p = it->buffer + it->offset;// 长度 0 时 p 指向当前位置（占位）
        }
        out->type = SERI_ITEM_STRING;
        out->v.s.p = p;
        out->v.s.len = cookie;
        return 1;
    case SERI_TYPE_LONG_STRING: {
        size_t slen;
        if (2 == cookie) {
            if (NULL == (p = _seri_rb_read(it, 2))) {
                return -1;
            }
            slen = (size_t)read_le16(p);
        } else if (4 == cookie) {
            if (NULL == (p = _seri_rb_read(it, 4))) {
                return -1;
            }
            slen = (size_t)read_le32(p);
        } else {
            return -1;
        }
        if (NULL == (p = _seri_rb_read(it, slen))) {
            return -1;
        }
        out->type = SERI_ITEM_STRING;
        out->v.s.p = p;
        out->v.s.len = slen;
        return 1;
    }
    case SERI_TYPE_ARRAY: {
        uint32_t array_n;
        if (cookie < SERI_MAX_COOKIE - 1) {
            array_n = cookie;
        } else {
            // 长 array 转义：后跟一个完整 INT
            const char *q = _seri_rb_read(it, 1);
            if (NULL == q) {
                return -1;
            }
            uint8_t ntag = (uint8_t)*q;
            uint8_t ntype = ntag & 0x7;
            uint8_t ncookie = ntag >> 3;
            if (SERI_TYPE_NUMBER != ntype || SERI_NUMBER_REAL == ncookie) {
                return -1;
            }
            int64_t v;
            if (0 != _seri_read_integer(it, ncookie, &v) || v < 0 || v > UINT32_MAX) {
                return -1;
            }
            array_n = (uint32_t)v;
        }
        out->type = SERI_ITEM_ARRAY_BEGIN;
        out->v.array_n = array_n;
        return 1;
    }
    default:
        return -1;
    }
}
