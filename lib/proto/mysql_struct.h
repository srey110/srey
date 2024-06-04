#ifndef MYSQL_STRUCT_H_
#define MYSQL_STRUCT_H_

#include "structs.h"

typedef struct mysql_ctx {
    int8_t id;
    int8_t protocol_ver;
    uint8_t cur_cmd;
    uint8_t server_charset;
    uint8_t client_charset;
    uint16_t port;
    uint16_t status_flags;
    int32_t status;
    uint32_t maxpack;
    uint32_t thread_id;
    uint32_t client_caps;
    uint32_t server_caps;
    SOCKET fd;
    uint64_t skid;
    struct evssl_ctx *evssl;
    char salt[20];
    char version[32];
    char plugin[32];
    char ip[IP_LENS];
    char user[64];
    char database[64];
    char password[128];
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
