#ifndef PGSQL_PARSE_H_
#define PGSQL_PARSE_H_

#include "protocol/pgsql/pgsql_struct.h"
#include "utils/binary.h"

// 解析 ErrorResponse / NoticeResponse，返回格式化错误描述字符串，调用方负责释放
char *_pgpack_error_notice(binary_ctx *breader);
// 给 DataRow 分内存：pg 当前有建好字段描述的 reader 时从它的块链切 total 字节并在块尾多分行数组
// [消息][补齐到 8][pgpack_row × 列数]，随 reader 释放；否则返回 NULL，由调用方单独分配
char *_pgpack_row_alloc(pgsql_ctx *pg, size_t total);
// 释放 pgpack_ctx 及其持有的内部数据
void _pgpack_free(pgpack_ctx *pgpack);
// 释放 pgsql_reader_ctx 内部数据（行数组与字段数组），不释放结构体本身
void _pgpack_reader_free(void *arg);
// 解析一个完整的服务端消息，更新 pgsql_ctx 状态，在 ReadyForQuery 时返回累积的 pgpack_ctx。
// breader->data 只有 DataRow('D') 与 NotificationResponse('A') 由本函数接管(释放或转交)，
// 其余类型本函数不留指向它的指针、也不释放，由调用方处置（可以是栈上缓冲）
pgpack_ctx *_pgpack_parser(pgsql_ctx *pg, binary_ctx *breader, ud_cxt *ud, int32_t *status);

#endif//PGSQL_PARSE_H_
