#ifndef MYSQL_STRUCT_H_
#define MYSQL_STRUCT_H_

#include "structs.h"
#include "binary.h"

typedef enum mysql_field_types {
    MYSQL_TYPE_DECIMAL,
    MYSQL_TYPE_TINY,
    MYSQL_TYPE_SHORT,
    MYSQL_TYPE_LONG,
    MYSQL_TYPE_FLOAT,
    MYSQL_TYPE_DOUBLE,
    MYSQL_TYPE_NULL,
    MYSQL_TYPE_TIMESTAMP,
    MYSQL_TYPE_LONGLONG,
    MYSQL_TYPE_INT24,
    MYSQL_TYPE_DATE,
    MYSQL_TYPE_TIME,
    MYSQL_TYPE_DATETIME,
    MYSQL_TYPE_YEAR,
    MYSQL_TYPE_NEWDATE,
    MYSQL_TYPE_VARCHAR,
    MYSQL_TYPE_BIT,
    MYSQL_TYPE_TIMESTAMP2,
    MYSQL_TYPE_DATETIME2,
    MYSQL_TYPE_TIME2,
    MYSQL_TYPE_TYPED_ARRAY,
    MYSQL_TYPE_INVALID = 243,
    MYSQL_TYPE_BOOL = 244,
    MYSQL_TYPE_JSON = 245,
    MYSQL_TYPE_NEWDECIMAL = 246,
    MYSQL_TYPE_ENUM = 247,
    MYSQL_TYPE_SET = 248,
    MYSQL_TYPE_TINY_BLOB = 249,
    MYSQL_TYPE_MEDIUM_BLOB = 250,
    MYSQL_TYPE_LONG_BLOB = 251,
    MYSQL_TYPE_BLOB = 252,
    MYSQL_TYPE_VAR_STRING = 253,
    MYSQL_TYPE_STRING = 254,
    MYSQL_TYPE_GEOMETRY = 255
}mysql_field_types;

typedef struct mysql_client_params {
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
}mysql_client_params;
typedef struct mysql_server_params {
    uint16_t status_flags;
    uint32_t caps;
    char salt[20];
    char plugin[32];
}mysql_server_params;
typedef struct mysql_binary_form {
    int32_t count;
    binary_ctx bitmap;
    binary_ctx type_name;
    binary_ctx values;
}mysql_binary_form;
typedef struct mysql_ctx {
    int8_t id;
    uint8_t cur_cmd;
    int32_t status;
    mysql_server_params server;
    mysql_client_params client;
    mysql_binary_form bform;
}mysql_ctx;

typedef struct mpack_ctx {
    uint8_t command;
    int8_t sequence_id;
    size_t payload_lens;
    char *payload;
    void *mpack;
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

#endif//MYSQL_STRUCT_H_
