#ifndef MYSQL_PARSE_H_
#define MYSQL_PARSE_H_

#include "binary.h"
#include "proto/mysql_pack.h"

typedef struct mpack_authv10 {
    uint32_t caps;//capability_flags
    size_t s1lens;//auth-plugin-data-part-1
    size_t s2lens;//auth-plugin-data-part-2 len
    char *svver;//server version
    char *salt1;//auth-plugin-data-part-1
    char *salt2;//auth-plugin-data-part-2
    char *auth_plugin;//auth_plugin_name
    char *payload;
}mpack_authv10;
typedef struct mysql_params {
    int8_t id;
    int32_t maxpack;
    uint32_t client_caps;
    size_t plens;
    struct evssl_ctx *evssl;
    char *user;
    char *password;
    char *database;
    char *charset;
    mpack_authv10 authv10;
}mysql_params;

void _mysql_set_fixed_lens_integer(binary_ctx *breader, size_t lens);
int32_t _mpack_ok(mysql_params *params, binary_ctx *breader, mpack_ok *ok);
int32_t _mpack_eof(mysql_params *params, binary_ctx *breader, mpack_eof *eof);
int32_t _mpack_err(mysql_params *params, binary_ctx *breader, mpack_err *err);
int32_t _mpack_auth_switch(mysql_params *params, binary_ctx *breader, mpack_auth_switch *auswitch);
int32_t _mpack_parser(mysql_params *params, binary_ctx *breader, mysql_pack_ctx *mpack);

#endif//MYSQL_PARSE_H_
