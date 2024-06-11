#ifndef MYSQL_STRUCT_H_
#define MYSQL_STRUCT_H_

#include "base/structs.h"
#include "utils/binary.h"
#include "containers/sarray.h"
#include "protocol/mysql/mysql_macro.h"

struct mpack_ctx;
typedef struct mysql_client_param {
    int8_t relink;
    uint8_t charset;
    uint16_t port;
    uint32_t maxpack;
    uint32_t caps;
    SOCKET fd;
    uint64_t skid;
    struct evssl_ctx *evssl;
    char ip[IP_LENS];
    char user[64];
    char database[64];
    char password[64];
}mysql_client_param;
typedef struct mysql_server_param {
    uint16_t status_flags;
    uint32_t caps;
    char salt[20];
    char plugin[32];
}mysql_server_param;
typedef struct mysql_ctx {
    int8_t id;
    int8_t parse_status;
    uint8_t cur_cmd;
    int16_t error_code;
    int32_t status;
    int64_t last_id;
    int64_t affected_rows;
    struct mpack_ctx *mpack;
    mysql_server_param server;
    mysql_client_param client;
    char error_msg[256];
}mysql_ctx;

typedef struct mysql_bind_ctx {
    int32_t count;
    binary_ctx bitmap;
    binary_ctx type;
    binary_ctx type_name;
    binary_ctx value;
}mysql_bind_ctx;

typedef struct mpack_ctx {
    int8_t sequence_id;
    mpack_type pack_type;
    char *payload;
    void *pack;
    void(*_free_mpack)(void *);
}mpack_ctx;
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
typedef struct mpack_field {
    uint8_t decimals;//max shown decimal digits
    uint8_t type;//enum_field_types
    uint16_t flags;//Flags as defined in Column Definition Flags
    int16_t character;//character set
    int32_t field_lens;//maximum length of the field
    char schema[64];//schema name
    char table[64];//virtual table name
    char org_table[64];//physical table name
    char name[64];//virtual column name
    char org_name[64];//physical column name
}mpack_field;
typedef struct mpack_row {
    int32_t nil;//1 : NULL
    buf_ctx val;//NULL and 0 == nil ""
    char *payload;
}mpack_row;
typedef struct mysql_reader_ctx {//Resultset
    mpack_type pack_type;
    int32_t field_count;
    int32_t index;
    mpack_field *fields;
    arr_ptr_ctx arr_rows;
}mysql_reader_ctx;
typedef struct mysql_stmt_ctx {
    uint16_t field_count;
    uint16_t params_count;
    int32_t index;
    int32_t stmt_id;
    mpack_field *params;
    mpack_field *fields;
    mysql_ctx *mysql;
}mysql_stmt_ctx;

#endif//MYSQL_STRUCT_H_
