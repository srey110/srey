#ifndef MONGO_PACK_H_
#define MONGO_PACK_H_

#include "protocol/mongo/mongo_struct.h"
//https://www.mongodb.com/zh-cn/docs/manual/reference/command/

void *mongo_pack_scram_client_first(mongo_ctx *mongo, const char *method, size_t *size);
void *mongo_pack_scram_client_final(mongo_ctx *mongo, int32_t convid, char *client_final, size_t *size);

void *mongo_pack_hello(mongo_ctx *mongo, char *options, size_t *size);
void *mongo_pack_ping(mongo_ctx *mongo, size_t *size);

void *mongo_pack_drop(mongo_ctx *mongo, char *options, size_t *size);
void *mongo_pack_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t *size);
void *mongo_pack_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t *size);
void *mongo_pack_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t *size);
void *mongo_pack_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t *size);
void *mongo_pack_find(mongo_ctx *mongo, char *filter, size_t flens, char *options, size_t *size);
void *mongo_pack_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options, size_t *size);
void *mongo_pack_getmore(mongo_ctx *mongo, int64_t cursorid, char *options, size_t *size);
void *mongo_pack_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options, size_t *size);
void *mongo_pack_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options, size_t *size);
void *mongo_pack_findandmodify(mongo_ctx *mongo, char *query, size_t qlens, int32_t remove, int32_t pipeline, char *update, size_t ulens,
    char *options, size_t *size);
void *mongo_pack_count(mongo_ctx *mongo, char *query, size_t qlens, char *options, size_t *size);

void *mongo_pack_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t *size);
void *mongo_pack_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t *size);

void *mongo_pack_startsession(mongo_ctx *mongo, size_t *size);
void *mongo_pack_refreshsession(mongo_session *session, size_t *size);
void *mongo_pack_endsession(mongo_session *session, size_t *size);
char *mongo_transaction_options(mongo_session *session);
void *mongo_pack_committransaction(mongo_session *session, char *options, size_t *size);
void *mongo_pack_aborttransaction(mongo_session *session, char *options, size_t *size);

#endif//MONGO_PACK_H_
