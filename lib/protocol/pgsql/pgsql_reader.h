#ifndef PGSQL_READER_H_
#define PGSQL_READER_H_

#include "protocol/pgsql/pgsql_struct.h"

/// <summary>
/// pgsql_reader_ctx 初始化
/// </summary>
/// <param name="pgpack">pgpack_ctx</param>
/// <param name="format">pgpack_format</param>
/// <returns>pgsql_reader_ctx NULL 失败</returns>
pgsql_reader_ctx *pgsql_reader_init(pgpack_ctx *pgpack, pgpack_format format);
/// <summary>
/// pgsql_reader_ctx 释放
/// </summary>
/// <param name="reader">pgsql_reader_ctx</param>
void pgsql_reader_free(pgsql_reader_ctx *reader);
/// <summary>
/// 数据行数
/// </summary>
/// <param name="reader">pgsql_reader_ctx</param>
/// <returns>行数</returns>
size_t pgsql_reader_size(pgsql_reader_ctx *reader);
/// <summary>
/// 移到第几条数据
/// </summary>
/// <param name="reader">pgsql_reader_ctx</param>
/// <param name="pos">条数</param>
void pgsql_reader_seek(pgsql_reader_ctx *reader, size_t pos);
/// <summary>
/// 是否有数据
/// </summary>
/// <param name="reader">pgsql_reader_ctx</param>
/// <returns>1 无数据 0 有数据</returns>
int32_t pgsql_reader_eof(pgsql_reader_ctx *reader);
/// <summary>
/// 下一条数据
/// </summary>
/// <param name="reader">pgsql_reader_ctx</param>
void pgsql_reader_next(pgsql_reader_ctx *reader);
pgpack_row *pgsql_reader_index(pgsql_reader_ctx *reader, int16_t index, pgpack_field **field);
pgpack_row *pgsql_reader_name(pgsql_reader_ctx *reader, const char *name, pgpack_field **field);

#endif//PGSQL_READER_H_
