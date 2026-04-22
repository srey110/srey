#include "protocol/pgsql/pgsql_bind.h"

void pgsql_bind_init(pgsql_bind_ctx *bind, uint16_t nparam) {
    bind->nparam = nparam;
    if (0 == bind->nparam) {
        return;
    }
    binary_init(&bind->format, NULL, 0, 0);
    binary_set_integer(&bind->format, bind->nparam, 2, 0);//参数格式代码数量
    binary_init(&bind->values, NULL, 0, 0);
    binary_set_integer(&bind->values, bind->nparam, 2, 0);//参数值数量
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
    binary_offset(&bind->format, 2);
    binary_offset(&bind->values, 2);
}
void pgsql_bind(pgsql_bind_ctx *bind, char *value, size_t lens, pgpack_format format) {
    if (0 == bind->nparam) {
        return;
    }
    binary_set_integer(&bind->format, format, 2, 0);//参数格式代码
    binary_set_integer(&bind->values, lens, 4, 0);//参数值的长度
    if (lens > 0) {
        binary_set_string(&bind->values, value, lens);//参数的值
    }
}
void pgsql_bind_bool(pgsql_bind_ctx *bind, int8_t value) {
    char b[1] = { value ? 1 : 0 };
    pgsql_bind(bind, b, 1, FORMAT_BINARY);
}
void pgsql_bind_int16(pgsql_bind_ctx *bind, int16_t value) {
    pack_integer((char *)&value, value, sizeof(value), 0);
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_int32(pgsql_bind_ctx *bind, int32_t value) {
    pack_integer((char *)&value, value, sizeof(value), 0);
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_int64(pgsql_bind_ctx *bind, int64_t value) {
    pack_integer((char *)&value, value, sizeof(value), 0);
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_float(pgsql_bind_ctx *bind, float value) {
    pack_float((char *)&value, value, 0);
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
void pgsql_bind_double(pgsql_bind_ctx *bind, double value) {
    pack_double((char *)&value, value, 0);
    pgsql_bind(bind, (char *)&value, sizeof(value), FORMAT_BINARY);
}
