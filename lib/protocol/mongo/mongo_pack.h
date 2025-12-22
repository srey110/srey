#ifndef MONGO_PACK_H_
#define MONGO_PACK_H_

#include "protocol/mongo/mongo_struct.h"

void *mongo_pack_msg(mongo_ctx *mongo, int32_t flag, int32_t kind, const char *docid,
    const uint8_t *docs, size_t dlens, int32_t *id, size_t *size);
void *mongo_pack_hello(mongo_ctx *mongo, int32_t *reqid, size_t *size);
void *mongo_pack_scram_client_first(mongo_ctx *mongo, const char *method, int32_t *reqid, size_t *size);
void *mongo_pack_scram_client_final(mongo_ctx *mongo, int32_t convid, char *client_final, int32_t *reqid, size_t *size);

#endif//MONGO_PACK_H_
