#ifndef MONGO_PARSE_H_
#define MONGO_PARSE_H_
#include "protocol/mongo/mongo_struct.h"

int32_t mongo_parse_auth_response(mgopack_ctx *mgopack, int32_t *convid, int32_t *done, char **payload, size_t *plens);
int64_t mongo_cursorid(mgopack_ctx *mgpack);
int32_t mongo_parse_check_error(mongo_ctx *mongo, mgopack_ctx *mgpack);
void mongo_set_error(mongo_ctx *mongo, const char *err, int32_t copy);
int32_t mongo_parse_startsession(mongo_ctx *mongo, mgopack_ctx *mgpack, char uid[UUID_LENS], int32_t *timeout);

#endif//MONGO_PARSE_H_
