#include "protocol/pgsql/pgsql_parse.h"

static char *_pgpack_error_notice(pgsql_ctx *pg, binary_ctx *breader) {
    char flag;
    char *tmp;
    binary_ctx bwriter;
    binary_init(&bwriter, NULL, 0, 0);
    for (;;) {
        flag = binary_get_int8(breader);
        tmp = binary_get_string(breader, 0);
        binary_set_int8(&bwriter, flag);
        binary_set_string(&bwriter, ": ", 2);
        if (breader->size - breader->offset > 1) {//1 OPCODE
            binary_set_va(&bwriter, "%s\r\n", tmp);
        } else {
            binary_set_string(&bwriter, tmp, 0);
            break;
        }
    }
    return bwriter.data;
}
char *_pgpack_error(pgsql_ctx *pg, binary_ctx *breader) {
    return _pgpack_error_notice(pg, breader);
}
char *_pgpack_notice(pgsql_ctx *pg, binary_ctx *breader) {
    return _pgpack_error_notice(pg, breader);
}
pgpack_ctx *_pgsql_parser(pgsql_ctx *pg, binary_ctx *breader, ud_cxt *ud, int32_t *status) {
   /* switch (code) {
    case 'E':
        break;
    default:

        break;
    }*/
    return NULL;
}
