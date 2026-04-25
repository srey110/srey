#ifndef PGSQL_PARSE_H_
#define PGSQL_PARSE_H_

#include "protocol/pgsql/pgsql_struct.h"
#include "utils/binary.h"

// 解析 ErrorResponse / NoticeResponse，返回格式化错误描述字符串，调用方负责释放
char *_pgpack_error_notice(binary_ctx *breader);
// 释放 pgpack_ctx 及其持有的内部数据
void _pgpack_free(pgpack_ctx *pgpack);
// 释放 pgsql_reader_ctx 内部数据（行数组与字段数组），不释放结构体本身
void _pgpack_reader_free(void *arg);
// 解析一个完整的服务端消息，更新 pgsql_ctx 状态，在 ReadyForQuery 时返回累积的 pgpack_ctx
pgpack_ctx *_pgpack_parser(pgsql_ctx *pg, binary_ctx *breader, ud_cxt *ud, int32_t *status);

#endif//PGSQL_PARSE_H_
