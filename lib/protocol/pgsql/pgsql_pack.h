#ifndef PGSQL_PACK_H_
#define PGSQL_PACK_H_

#include "utils/binary.h"
#include "protocol/pgsql/pgsql_struct.h"

/// <summary>
/// pgsql_bind_ctx初始化
/// </summary>
/// <param name="bind">pgsql_bind_ctx</param>
/// <param name="nparam">参数数量</param>
void pgsql_bind_init(pgsql_bind_ctx *bind, uint16_t nparam);
/// <summary>
/// pgsql_bind_ctx释放
/// </summary>
/// <param name="bind">pgsql_bind_ctx</param>
void pgsql_bind_free(pgsql_bind_ctx *bind);
/// <summary>
/// pgsql_bind_ctx清空
/// </summary>
/// <param name="bind">pgsql_bind_ctx</param>
void pgsql_bind_clear(pgsql_bind_ctx *bind);
/// <summary>
/// 绑定参数
/// </summary>
/// <param name="bind">pgsql_bind_ctx</param>
/// <param name="index">序号[0 - nparam)</param>
/// <param name="val">值</param>
/// <param name="lens">值长度</param>
/// <param name="format">格式 FORMAT_TEXT FORMAT_BINARY</param>
void pgsql_bind(pgsql_bind_ctx *bind, uint16_t index, char *val, size_t lens, int16_t format);

void pgsql_pack_start(binary_ctx *bwriter, int8_t code);
void pgsql_pack_end(binary_ctx *bwriter);
size_t pgsql_pack_append_start(binary_ctx *bwriter, int8_t code);
void pgsql_pack_append_end(binary_ctx *bwriter, size_t offset);

void *pgsql_pack_terminate(size_t *size);
void *pgsql_pack_query(const char *sql, size_t *size);
void *pgsql_pack_stmt_prepare(const char *name, const char *sql, int16_t nparam, uint32_t *oids, size_t *size);
void *pgsql_pack_stmt_execute(const char *name, pgsql_bind_ctx *bind, int16_t resultformat, size_t *size);
void *pgsql_pack_stmt_close(const char *name, size_t *size);

#endif//PGSQL_PACK_H_
