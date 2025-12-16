#ifndef PGSQL_PACK_H_
#define PGSQL_PACK_H_

#include "utils/binary.h"
#include "protocol/pgsql/pgsql_struct.h"

//命令打包辅助函数
void pgsql_pack_start(binary_ctx *bwriter, int8_t code);
void pgsql_pack_end(binary_ctx *bwriter);
size_t pgsql_pack_append_start(binary_ctx *bwriter, int8_t code);
void pgsql_pack_append_end(binary_ctx *bwriter, size_t offset);
//命令打包
void *pgsql_pack_terminate(size_t *size);
void *pgsql_pack_query(const char *sql, size_t *size);
void *pgsql_pack_stmt_prepare(const char *name, const char *sql, int16_t nparam, uint32_t *oids, size_t *size);
void *pgsql_pack_stmt_execute(const char *name, pgsql_bind_ctx *bind, pgpack_format resultformat, size_t *size);
void *pgsql_pack_stmt_close(const char *name, size_t *size);

#endif//PGSQL_PACK_H_
