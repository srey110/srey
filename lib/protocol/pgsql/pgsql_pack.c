#include "protocol/pgsql/pgsql_pack.h"

void pgsql_bind_init(pgsql_bind_ctx *bind, uint16_t nparam) {
    ZERO(bind, sizeof(pgsql_bind_ctx));
    bind->nparam = nparam;
    if (0 == bind->nparam) {
        return;
    }
    CALLOC(bind->values, 1, sizeof(buf_ctx) * bind->nparam);
    CALLOC(bind->format, 1, sizeof(int16_t) * bind->nparam);
}
void pgsql_bind_free(pgsql_bind_ctx *bind) {
    if (0 == bind->nparam) {
        return;
    }
    FREE(bind->values);
    FREE(bind->format);
}
void pgsql_bind_clear(pgsql_bind_ctx *bind) {
    if (0 == bind->nparam) {
        return;
    }
    ZERO(bind->values, sizeof(buf_ctx) * bind->nparam);
    ZERO(bind->format, sizeof(int16_t) * bind->nparam);
}
void pgsql_bind(pgsql_bind_ctx *bind, uint16_t index, char *val, size_t lens, int16_t format) {
    ASSERTAB(index >= 0 && index < bind->nparam, "out of range.");
    bind->values[index].data = val;
    bind->values[index].lens = lens;
    bind->format[index] = format;
}
void pgsql_pack_start(binary_ctx *bwriter, int8_t code) {
    binary_init(bwriter, NULL, 0, 0);
    binary_set_int8(bwriter, code);
    binary_set_skip(bwriter, 4);
}
void pgsql_pack_end(binary_ctx *bwriter) {
    size_t size = bwriter->offset;
    binary_offset(bwriter, 1);
    binary_set_integer(bwriter, size - 1, 4, 0);
    binary_offset(bwriter, size);
}
size_t pgsql_pack_append_start(binary_ctx *bwriter, int8_t code) {
    binary_set_int8(bwriter, code);
    size_t offset = bwriter->offset;
    binary_set_skip(bwriter, 4);
    return offset;
}
void pgsql_pack_append_end(binary_ctx *bwriter, size_t offset) {
    size_t size = bwriter->offset;
    binary_offset(bwriter, offset);
    binary_set_integer(bwriter, size - offset, 4, 0);
    binary_offset(bwriter, size);
}
void *pgsql_pack_terminate(size_t *size) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'X');//Terminate Byte1('X') Int32(4)
    pgsql_pack_end(&bwriter);
    *size = bwriter.offset;
    return bwriter.data;
}
void *pgsql_pack_query(const char *sql, size_t *size) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'Q');//Query Byte1('Q') Int32 String
    binary_set_string(&bwriter, sql, 0);
    pgsql_pack_end(&bwriter);
    *size = bwriter.offset;
    return bwriter.data;
}
void *pgsql_pack_stmt_prepare(const char *name, const char *sql, int16_t nparam, uint32_t *oids, size_t *size) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'P');//Parse Byte1('P') Int32 String String Int16 [Int32]
    if (!EMPTYSTR(name)) {
        binary_set_string(&bwriter, name, 0);//目标准备语句的名称
    } else {
        binary_set_int8(&bwriter, 0);
    }
    binary_set_string(&bwriter, sql, 0);//要解析的查询字符串
    if (nparam > 0
        && NULL != oids) {
        binary_set_integer(&bwriter, nparam, 2, 0);//指定的参数数据类型的数量
        for (int16_t i = 0; i < nparam; i++) {
            binary_set_integer(&bwriter, oids[i], 4, 0);//数据类型的对象ID
        }
    } else {
        binary_set_integer(&bwriter, 0, 2, 0);
    }
    pgsql_pack_end(&bwriter);
    size_t offset = pgsql_pack_append_start(&bwriter, 'S');//Sync  Byte1('S')  Int32(4)
    pgsql_pack_append_end(&bwriter, offset);
    *size = bwriter.offset;
    return bwriter.data;
}
void *pgsql_pack_stmt_execute(const char *name, pgsql_bind_ctx *bind, int16_t resultformat, size_t *size) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'B');//Bind Byte1('B') Int32 String String Int16 [Int16] Int16 [Int32 Byten] Int16 [Int16]
    binary_set_string(&bwriter, "", 0);//目标门户的名称
    binary_set_string(&bwriter, name, 0);//源准备语句的名称
    if (NULL == bind
        || 0 == bind->nparam) {
        binary_set_integer(&bwriter, 0, 2, 0);//参数格式代码的数量
        binary_set_integer(&bwriter, 0, 2, 0);//参数格式代码
    } else {
        uint16_t i;
        binary_set_integer(&bwriter, bind->nparam, 2, 0);//参数格式代码的数量
        for (i = 0; i < bind->nparam; i++) {
            binary_set_integer(&bwriter, bind->format[i], 2, 0);//参数格式代码
        }
        binary_set_integer(&bwriter, bind->nparam, 2, 0);//参数值的数量
        for (i = 0; i < bind->nparam; i++) {
            binary_set_integer(&bwriter, bind->values[i].lens, 4, 0);//参数值的长度
            if (bind->values[i].lens > 0) {
                binary_set_string(&bwriter, bind->values[i].data, bind->values[i].lens);//参数的值
            }
        }
    }
    binary_set_integer(&bwriter, 1, 2, 0);//结果列格式代码数量
    binary_set_integer(&bwriter, resultformat, 2, 0);//结果列格式代码
    pgsql_pack_end(&bwriter);
    size_t offset = pgsql_pack_append_start(&bwriter, 'D');//Describe Byte1('D') Int32 Byte1 String
    binary_set_int8(&bwriter, 'S');//'S':描述一个准备好的语句  'P':描述一个门户。
    binary_set_string(&bwriter, name, 0);
    pgsql_pack_append_end(&bwriter, offset);
    offset = pgsql_pack_append_start(&bwriter, 'E'); //Execute Byte1('E') Int32 String Int32
    binary_set_string(&bwriter, "", 0);//要执行的门户的名称
    binary_set_integer(&bwriter, 0, 4, 0);//返回的最大行数 0“没有限制”
    pgsql_pack_append_end(&bwriter, offset);
    offset = pgsql_pack_append_start(&bwriter, 'S');//Sync  Byte1('S')  Int32(4)
    pgsql_pack_append_end(&bwriter, offset);
    *size = bwriter.offset;
    return bwriter.data;
}
void *pgsql_pack_stmt_close(const char *name, size_t *size) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'C');//Byte1('C') Int32  Byte1 String
    binary_set_int8(&bwriter, 'S');//'S':描述一个准备好的语句  'P':描述一个门户。
    binary_set_string(&bwriter, name, 0);
    pgsql_pack_end(&bwriter);
    size_t offset = pgsql_pack_append_start(&bwriter, 'S');//Sync  Byte1('S')  Int32(4)
    pgsql_pack_append_end(&bwriter, offset);
    *size = bwriter.offset;
    return bwriter.data;
}
