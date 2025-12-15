#ifndef PGSQL_PACK_H_
#define PGSQL_PACK_H_

#include "utils/binary.h"
#include "protocol/pgsql/pgsql_struct.h"

void pgsql_bind_init(pgsql_bind_ctx *bind, uint16_t nparam);
void pgsql_bind_free(pgsql_bind_ctx *bind);
void pgsql_bind_clear(pgsql_bind_ctx *bind);
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
