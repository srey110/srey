#ifndef MYSQL_PARSE_H_
#define MYSQL_PARSE_H_

#include "protocol/mysql/mysql_struct.h"
#include "utils/buffer.h"

/// <summary>
/// 从 STMT_PREPARE 响应包中初始化并提取预处理语句上下文
/// </summary>
/// <param name="mpack">mpack_ctx，类型必须为 MPACK_STMT_PREPARE</param>
/// <returns>mysql_stmt_ctx，失败返回 NULL</returns>
mysql_stmt_ctx *mysql_stmt_init(mpack_ctx *mpack);

// 内部函数：释放预处理语句上下文中的参数和字段数组
void _mpack_stm_free(void *pack);
// 内部函数：释放结果集读取器中所有行数据和字段数组
void _mpack_reader_free(void *pack);
// 内部函数：从接收缓冲区读取并分配一个完整 MySQL payload，数据不足时设置 PROT_MOREDATA
char *_mysql_payload(mysql_ctx *mysql, buffer_ctx *buf, size_t *payload_lens, int32_t *status);

// 内部函数：解析 OK 响应包，更新 mysql->last_id 和 mysql->affected_rows
void _mpack_ok(mysql_ctx *mysql, binary_ctx *breader, mpack_ok *ok);
// 内部函数：解析 ERROR 响应包，更新 mysql->error_code 和 mysql->error_msg
void _mpack_err(mysql_ctx *mysql, binary_ctx *breader, mpack_err *err);
// 内部函数：根据 mysql->cur_cmd 分发并解析响应包，返回完整解析的 mpack_ctx
mpack_ctx *_mpack_parser(mysql_ctx *mysql, buffer_ctx *buf, binary_ctx *breader, int32_t *status);

#endif//MYSQL_PARSE_H_
