#include "protocol/pgsql/pgsql_pack.h"

void pgsql_pack_start(binary_ctx *bwriter, int8_t code) {
    binary_init(bwriter, NULL, 0, 0);
    binary_set_int8(bwriter, code);    // 写入消息类型码
    binary_set_skip(bwriter, 4);       // 预留 4 字节消息体长度字段
}
void pgsql_pack_end(binary_ctx *bwriter) {
    size_t size = bwriter->offset;
    binary_offset(bwriter, 1);                          // 跳过类型码，定位到长度字段
    binary_set_integer(bwriter, size - 1, 4, 0);        // 回填消息体长度（不含类型码字节）
    binary_offset(bwriter, size);                       // 恢复偏移到消息末尾
}
size_t pgsql_pack_append_start(binary_ctx *bwriter, int8_t code) {
    binary_set_int8(bwriter, code);    // 追加子消息类型码
    size_t offset = bwriter->offset;   // 记录长度字段的起始偏移
    binary_set_skip(bwriter, 4);       // 预留 4 字节长度字段
    return offset;
}
void pgsql_pack_append_end(binary_ctx *bwriter, size_t offset) {
    size_t size = bwriter->offset;
    binary_offset(bwriter, offset);                     // 定位到长度字段位置
    binary_set_integer(bwriter, size - offset, 4, 0);   // 回填子消息体长度
    binary_offset(bwriter, size);                       // 恢复偏移到消息末尾
}
void *pgsql_pack_terminate(size_t *size) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'X'); // Terminate：Byte1('X') Int32(4)
    pgsql_pack_end(&bwriter);
    *size = bwriter.offset;
    return bwriter.data;
}
void *pgsql_pack_query(const char *sql, size_t *size) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'Q'); // Query：Byte1('Q') Int32 String
    binary_set_string(&bwriter, sql, 0);
    pgsql_pack_end(&bwriter);
    *size = bwriter.offset;
    return bwriter.data;
}
void *pgsql_pack_stmt_prepare(const char *name, const char *sql, int16_t nparam, uint32_t *oids, size_t *size) {
    binary_ctx bwriter;
    pgsql_pack_start(&bwriter, 'P'); // Parse：Byte1('P') Int32 String String Int16 [Int32]
    if (!EMPTYSTR(name)) {
        binary_set_string(&bwriter, name, 0); // 目标预处理语句名称
    } else {
        binary_set_int8(&bwriter, 0); // 空名称表示匿名预处理语句
    }
    binary_set_string(&bwriter, sql, 0); // 要解析的 SQL 查询字符串
    if (nparam > 0
        && NULL != oids) {
        binary_set_integer(&bwriter, nparam, 2, 0); // 指定的参数数据类型数量
        for (int16_t i = 0; i < nparam; i++) {
            binary_set_integer(&bwriter, oids[i], 4, 0); // 各参数的类型 OID
        }
    } else {
        binary_set_integer(&bwriter, 0, 2, 0); // 无参数类型约束
    }
    pgsql_pack_end(&bwriter);
    // 追加 Sync 消息：Byte1('S') Int32(4)
    size_t offset = pgsql_pack_append_start(&bwriter, 'S');
    pgsql_pack_append_end(&bwriter, offset);
    *size = bwriter.offset;
    return bwriter.data;
}
void *pgsql_pack_stmt_execute(const char *name, pgsql_bind_ctx *bind, pgpack_format resultformat, size_t *size) {
    binary_ctx bwriter;
    // Bind：Byte1('B') Int32 String String Int16 [Int16] Int16 [Int32 Byten] Int16 [Int16]
    pgsql_pack_start(&bwriter, 'B');
    binary_set_string(&bwriter, "", 0);   // 目标门户名称（空字符串表示未命名门户）
    binary_set_string(&bwriter, name, 0); // 源预处理语句名称
    if (NULL == bind
        || 0 == bind->nparam) {
        binary_set_integer(&bwriter, 0, 2, 0); // 参数格式代码数量为 0
        binary_set_integer(&bwriter, 0, 2, 0); // 参数值数量为 0
    } else {
        binary_set_string(&bwriter, bind->format.data, bind->format.offset); // 参数格式码序列
        binary_set_string(&bwriter, bind->values.data, bind->values.offset); // 参数值序列
    }
    binary_set_integer(&bwriter, 1, 2, 0);              // 结果列格式代码数量为 1（统一格式）
    binary_set_integer(&bwriter, resultformat, 2, 0);   // 结果列格式代码
    pgsql_pack_end(&bwriter);
    // Describe：Byte1('D') Int32 Byte1 String
    size_t offset = pgsql_pack_append_start(&bwriter, 'D');
    binary_set_int8(&bwriter, 'S');       // 'S' 表示描述预处理语句，'P' 表示描述门户
    binary_set_string(&bwriter, name, 0);
    pgsql_pack_append_end(&bwriter, offset);
    // Execute：Byte1('E') Int32 String Int32
    offset = pgsql_pack_append_start(&bwriter, 'E');
    binary_set_string(&bwriter, "", 0);          // 要执行的门户名称
    binary_set_integer(&bwriter, 0, 4, 0);       // 最大返回行数，0 表示不限制
    pgsql_pack_append_end(&bwriter, offset);
    // Sync：Byte1('S') Int32(4)
    offset = pgsql_pack_append_start(&bwriter, 'S');
    pgsql_pack_append_end(&bwriter, offset);
    *size = bwriter.offset;
    return bwriter.data;
}
void *pgsql_pack_stmt_close(const char *name, size_t *size) {
    binary_ctx bwriter;
    // Close：Byte1('C') Int32 Byte1 String
    pgsql_pack_start(&bwriter, 'C');
    binary_set_int8(&bwriter, 'S');       // 'S' 关闭预处理语句，'P' 关闭门户
    binary_set_string(&bwriter, name, 0);
    pgsql_pack_end(&bwriter);
    // Sync：Byte1('S') Int32(4)
    size_t offset = pgsql_pack_append_start(&bwriter, 'S');
    pgsql_pack_append_end(&bwriter, offset);
    *size = bwriter.offset;
    return bwriter.data;
}
