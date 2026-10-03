#include "protocol/pgsql/pgsql_bind.h"

#define PGSQL_BIND_VALUES_INIT 256// 值区起步容量的下限
#define PGSQL_BIND_PARAM_EST 16// 值区按每个参数这么多字节估：4 字节长度加值，int4/int8/float8/timestamp 4~8 字节、短文本十几字节

// 值区起点：Int16 格式码数 + nparam 个格式码 + Int16 参数值数
static inline size_t _pgsql_bind_values_at(const pgsql_bind_ctx *bind) {
    return 4 + (size_t)bind->nparam * 2;
}
void pgsql_bind_init(pgsql_bind_ctx *bind, uint16_t nparam) {
    bind->nparam = nparam;
    bind->count = 0;
    if (0 == bind->nparam) {
        ZERO(&bind->buf, sizeof(bind->buf));
        return;
    }
    size_t at = _pgsql_bind_values_at(bind);
    size_t vals = (size_t)nparam * PGSQL_BIND_PARAM_EST;// 值区按参数个数估，免得参数多时反复翻倍扩容
    binary_init_write(&bind->buf, at + (vals > PGSQL_BIND_VALUES_INIT ? vals : PGSQL_BIND_VALUES_INIT), 0);
    binary_set_integer(&bind->buf, bind->nparam, 2, 0); // 参数格式代码数量
    binary_set_skip(&bind->buf, at - 4);
    binary_set_integer(&bind->buf, bind->nparam, 2, 0); // 参数值数量
}
void pgsql_bind_free(pgsql_bind_ctx *bind) {
    if (0 == bind->nparam) {
        return;
    }
    binary_free(&bind->buf);
    bind->nparam = 0;
    bind->count = 0;
}
void pgsql_bind_clear(pgsql_bind_ctx *bind) {
    if (0 == bind->nparam) {
        return;
    }
    // 回退到值区起点，保留两个参数数量；格式码槽下次按下标重写
    bind->count = 0;
    binary_offset(&bind->buf, _pgsql_bind_values_at(bind));
}
// 占下一个参数的格式码槽。多绑时把 count 顶到 nparam + 1 让组包拒，本次不写
static inline int32_t _pgsql_bind_format(pgsql_bind_ctx *bind, pgpack_format format) {
    char *p;
    if (bind->count >= bind->nparam) {
        bind->count = (uint32_t)bind->nparam + 1;
        return ERR_FAILED;
    }
    p = bind->buf.data + 2 + (size_t)bind->count * 2;
    write_be16(p, (uint16_t)format);// 格式码 2 字节
    bind->count++;
    return ERR_OK;
}
// 同 pgsql_bind，本文件的各 pgsql_bind_xxx 都走它
static inline void _pgsql_bind_put(pgsql_bind_ctx *bind, const char *value, size_t lens, pgpack_format format) {
    if (0 == bind->nparam
        || ERR_OK != _pgsql_bind_format(bind, format)) {
        return;
    }
    binary_set_integer(&bind->buf, lens, 4, 0); // 追加该参数值的字节长度
    if (lens > 0) {
        binary_set_binary(&bind->buf, value, lens); // 追加参数值数据
    }
}
void pgsql_bind(pgsql_bind_ctx *bind, char *value, size_t lens, pgpack_format format) {
    _pgsql_bind_put(bind, value, lens, format);
}
void pgsql_bind_bool(pgsql_bind_ctx *bind, int8_t value) {
    char b[1] = { value ? 1 : 0 };
    _pgsql_bind_put(bind, b, 1, FORMAT_BINARY);
}
void pgsql_bind_int16(pgsql_bind_ctx *bind, int16_t value) {
    write_be16(&value, (uint16_t)value); // 转为大端序
    _pgsql_bind_put(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_int32(pgsql_bind_ctx *bind, int32_t value) {
    write_be32(&value, (uint32_t)value); // 转为大端序
    _pgsql_bind_put(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_int64(pgsql_bind_ctx *bind, int64_t value) {
    write_be64(&value, (uint64_t)value); // 转为大端序
    _pgsql_bind_put(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_float(pgsql_bind_ctx *bind, float value) {
    pack_float((char *)&value, value, 0); // 转为大端序
    _pgsql_bind_put(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_double(pgsql_bind_ctx *bind, double value) {
    pack_double((char *)&value, value, 0); // 转为大端序
    _pgsql_bind_put(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_null(pgsql_bind_ctx *bind) {
    // NULL 值：格式码无实际意义，约定写 FORMAT_TEXT；长度字段写 -1（0xFFFFFFFF），无后续值字节
    if (0 == bind->nparam
        || ERR_OK != _pgsql_bind_format(bind, FORMAT_TEXT)) {
        return;
    }
    binary_set_integer(&bind->buf, -1, 4, 0);
}
void pgsql_bind_text(pgsql_bind_ctx *bind, const char *value, size_t lens) {
    // TEXT/VARCHAR/BPCHAR：UTF-8 字节流，直接以文本格式传递
    _pgsql_bind_put(bind, value, lens, FORMAT_TEXT);
}
void pgsql_bind_bytea(pgsql_bind_ctx *bind, const char *value, size_t lens) {
    // BYTEA：原始字节，以二进制格式直接传递
    _pgsql_bind_put(bind, value, lens, FORMAT_BINARY);
}
void pgsql_bind_timestamp(pgsql_bind_ctx *bind, int64_t usec) {
    // TIMESTAMP：相对 PostgreSQL 纪元（2000-01-01 00:00:00）的微秒数，大端序 int64
    write_be64(&usec, (uint64_t)usec);
    _pgsql_bind_put(bind, (char *)&usec, sizeof(usec), FORMAT_BINARY);
}
void pgsql_bind_timestamptz(pgsql_bind_ctx *bind, int64_t usec) {
    // TIMESTAMPTZ：二进制编码与 TIMESTAMP 相同（均为相对 PG UTC 纪元的微秒数）
    pgsql_bind_timestamp(bind, usec);
}
void pgsql_bind_date(pgsql_bind_ctx *bind, int32_t days) {
    // DATE：相对 PostgreSQL 纪元（2000-01-01）的天数，大端序 int32
    write_be32(&days, (uint32_t)days);
    _pgsql_bind_put(bind, (char *)&days, sizeof(days), FORMAT_BINARY);
}
void pgsql_bind_uuid(pgsql_bind_ctx *bind, const char uuid[16]) {
    // UUID：16 字节原始 UUID，网络字节序，以二进制格式传递
    _pgsql_bind_put(bind, uuid, 16, FORMAT_BINARY);
}
