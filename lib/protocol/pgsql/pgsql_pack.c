#include "protocol/pgsql/pgsql_pack.h"
#include "utils/binary.h"

void *pgsql_pack_quit(pgsql_ctx *pg, size_t *size) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, 'X');//Terminate
    binary_set_integer(&bwriter, 4, 4, 0);
    *size = bwriter.offset;
    return bwriter.data;
}
void *pgsql_pack_query(pgsql_ctx *pg, const char *sql, size_t *size) {
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    binary_set_int8(&bwriter, 'Q');//Query
    binary_set_skip(&bwriter, 4);
    binary_set_string(&bwriter, sql, 0);
    *size = bwriter.offset;
    binary_offset(&bwriter, 1);
    binary_set_integer(&bwriter, (*size) - 1, 4, 0);
    return bwriter.data;
}
