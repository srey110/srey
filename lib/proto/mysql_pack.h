#ifndef MYSQL_PACK_H_
#define MYSQL_PACK_H_

#include "structs.h"

typedef struct mysql_pack_ctx {
    uint8_t command;
    int8_t sequence_id;
    size_t payload_lens;
    char *payload;
    void *mpack;
    void(*_free_mpack)(void *);
}mysql_pack_ctx;
typedef struct mpack_ok {
    int16_t status_flags;
    int16_t warnings;
    int64_t affected_rows;
    int64_t last_insert_id;
}mpack_ok;
typedef struct mpack_eof {
    int16_t warnings;
    int16_t status_flags;
}mpack_eof;
typedef struct mpack_err {
    int16_t error_code;
    buf_ctx error_msg;
}mpack_err;
typedef struct mpack_auth_switch {
    char *plugin;
    buf_ctx provided;
}mpack_auth_switch;

#endif//MYSQL_PACK_H_
