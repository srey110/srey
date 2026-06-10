#include "protocol/pgsql/pgsql_bind.h"

void pgsql_bind_init(pgsql_bind_ctx *bind, uint16_t nparam) {
    bind->nparam = nparam;
    if (0 == bind->nparam) {
        return;
    }
    binary_init(&bind->format, NULL, 0, 0);
    binary_set_integer(&bind->format, bind->nparam, 2, 0); // 写入参数格式代码数量
    binary_init(&bind->values, NULL, 0, 0);
    binary_set_integer(&bind->values, bind->nparam, 2, 0); // 写入参数值数量
}
void pgsql_bind_free(pgsql_bind_ctx *bind) {
    if (0 == bind->nparam) {
        return;
    }
    FREE(bind->format.data);
    FREE(bind->values.data);
}
void pgsql_bind_clear(pgsql_bind_ctx *bind) {
    if (0 == bind->nparam) {
        return;
    }
    // 回退到数量字段之后，保留头部的参数数量，清空已绑定的格式码和值
    binary_offset(&bind->format, 2);
    binary_offset(&bind->values, 2);
}
void pgsql_bind(pgsql_bind_ctx *bind, char *value, size_t lens, pgpack_format format) {
    if (0 == bind->nparam) {
        return;
    }
    binary_set_integer(&bind->format, format, 2, 0); // 追加该参数的格式代码
    binary_set_integer(&bind->values, lens, 4, 0);   // 追加该参数值的字节长度
    if (lens > 0) {
        binary_set_string(&bind->values, value, lens); // 追加参数值数据
    }
}
void pgsql_bind_bool(pgsql_bind_ctx *bind, int8_t value) {
    char b[1] = { value ? 1 : 0 };
    pgsql_bind(bind, b, 1, FORMAT_BINARY);
}
void pgsql_bind_int16(pgsql_bind_ctx *bind, int16_t value) {
    pack_integer((char *)&value, value, sizeof(value), 0); // 转为大端序
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_int32(pgsql_bind_ctx *bind, int32_t value) {
    pack_integer((char *)&value, value, sizeof(value), 0); // 转为大端序
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_int64(pgsql_bind_ctx *bind, int64_t value) {
    pack_integer((char *)&value, value, sizeof(value), 0); // 转为大端序
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_float(pgsql_bind_ctx *bind, float value) {
    pack_float((char *)&value, value, 0); // 转为大端序
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_double(pgsql_bind_ctx *bind, double value) {
    pack_double((char *)&value, value, 0); // 转为大端序
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_null(pgsql_bind_ctx *bind) {
    if (0 == bind->nparam) {
        return;
    }
    // NULL 值：格式码无实际意义，约定写 FORMAT_TEXT；长度字段写 -1（0xFFFFFFFF），无后续值字节
    binary_set_integer(&bind->format, FORMAT_TEXT, 2, 0);
    binary_set_integer(&bind->values, -1, 4, 0);
}
void pgsql_bind_text(pgsql_bind_ctx *bind, const char *value, size_t lens) {
    // TEXT/VARCHAR/BPCHAR：UTF-8 字节流，直接以文本格式传递
    pgsql_bind(bind, (char *)value, lens, FORMAT_TEXT);
}
void pgsql_bind_bytea(pgsql_bind_ctx *bind, const char *value, size_t lens) {
    // BYTEA：原始字节，以二进制格式直接传递
    pgsql_bind(bind, (char *)value, lens, FORMAT_BINARY);
}
void pgsql_bind_timestamp(pgsql_bind_ctx *bind, int64_t usec) {
    // TIMESTAMP：相对 PostgreSQL 纪元（2000-01-01 00:00:00）的微秒数，大端序 int64
    pack_integer((char *)&usec, (uint64_t)usec, sizeof(usec), 0);
    pgsql_bind(bind, (char *)&usec, sizeof(usec), FORMAT_BINARY);
}
void pgsql_bind_timestamptz(pgsql_bind_ctx *bind, int64_t usec) {
    // TIMESTAMPTZ：二进制编码与 TIMESTAMP 相同（均为相对 PG UTC 纪元的微秒数）
    pgsql_bind_timestamp(bind, usec);
}
void pgsql_bind_date(pgsql_bind_ctx *bind, int32_t days) {
    // DATE：相对 PostgreSQL 纪元（2000-01-01）的天数，大端序 int32
    pack_integer((char *)&days, (uint64_t)(uint32_t)days, sizeof(days), 0);
    pgsql_bind(bind, (char *)&days, sizeof(days), FORMAT_BINARY);
}
void pgsql_bind_uuid(pgsql_bind_ctx *bind, const char uuid[16]) {
    // UUID：16 字节原始 UUID，网络字节序，以二进制格式传递
    pgsql_bind(bind, (char *)uuid, 16, FORMAT_BINARY);
}
