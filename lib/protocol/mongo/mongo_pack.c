#include "protocol/mongo/mongo_pack.h"
#include "serial/bson.h"
#include "utils/utils.h"
#include "utils/binary.h"
#include "crypt/scram.h"
#include "protocol/prots_pub.h"

// OP_MSG 头里 flagBits 的字节偏移：size / reqid / respto / opcode 各占 4 字节，见 _mongo_pack_msg
#define MSG_FLAGS_OFF 16
#define MONGO_MSG_HDR 21 // OP_MSG 头：size/reqid/respto/opcode/flags 各 4 字节 + kind 1 字节
// commitTransaction / abortTransaction 按规范只能发往 admin 库，与连接当前的 $db 无关；
// 发错库服务端回 code 13 Unauthorized "may only be run against the admin database"
#define MONGO_TXN_DB "admin"
#define BSON_HEADROOM 256 // 大消息 cap 估算余量（命令名+集合名+元数据+session options 等）
// 拼接 options：源文档超单包上限、或结构不合法(bson_cat 拒收)时整条命令作废——
// 继续打包会发出缺 options 的命令，服务端照常执行并返回错误结果集。
// *size 显式置 0：调用方按"返回非 NULL 才读 size"约定，早退路径不能留未初始化值
// 两道拒绝分开报:一条是文档超上限,一条是 BSON 结构不合法(判据见 bson_cat),合成一条会互相误报
#define MONGO_PACK_CAT(doc, lens) do { \
        if (0 != _mongo_cap_toolong((size_t)(lens))) { \
            *size = 0; \
            BSON_FREE(&bson); \
            return NULL; \
        } \
        if (ERR_OK != bson_cat(&bson, (doc), (lens))) { \
            LOG_ERROR("mongo document rejected: malformed bson, %zu bytes.", (size_t)(lens)); \
            *size = 0; \
            BSON_FREE(&bson); \
            return NULL; \
        } \
    } while (0)
// 必填的数组字段。空缓冲拒整条命令，早退四步与 MONGO_PACK_CAT 一致：
// bson_append_array 收到零长度只写 type + key、不写数组体，整篇 BSON 从这个元素起就解不开，
// 而这种包发出去服务端多半照收不误，错位要到后面某个字段才暴露
#define MONGO_PACK_ARR(name, doc, lens) do { \
        if (EMPTYPTR((doc), (lens))) { \
            LOG_ERROR("mongo %s rejected: empty document.", (name)); \
            *size = 0; \
            BSON_FREE(&bson); \
            return NULL; \
        } \
        bson_append_array(&bson, (name), (doc), (lens)); \
    } while (0)
// 组包三段式：MONGO_PACK_BEGIN 开头，中间按需 MONGO_PACK_CAT 拼外部文档，末尾一个 RETURN。
// 中段与收尾必须配对，配错编译期就报（判据见 MONGO_PACK_RETURN_TXN）：
//   事务内 CRUD            TRANSACTION_OPTIONS_START → MONGO_PACK_RETURN_TXN
//   带事务上下文不开事务    TRANSACTION_OPTIONS       → MONGO_PACK_RETURN
//   不涉事务                无                        → MONGO_PACK_RETURN

// 函数开头：声明并初始化局部 bson_ctx bson（必须置于函数体顶部）。要求函数有名为 size 的出参。
// cap：BSON 预估容量，0=默认；大消息传 dlens + BSON_HEADROOM 消除 doubling 重分配。宏内只求值一次。
// 超单包上限在这里就拒：cap 正是那几个大入参的长度，等到 _mongo_pack_msg 判总长时源数据
// 已经被全量分配并拷贝过，内存不够时分配器直接终止进程而不是返 NULL。
// 文档前预留 MONGO_MSG_HDR 字节，收尾时就地填 OP_MSG 头，正文不再另拷一份
#define MONGO_PACK_BEGIN(cap) MONGO_PACK_BEGIN2(cap, 0)
// 同 MONGO_PACK_BEGIN，extra 只加进首块容量、不进超限闸门(传 options 与事务 session options 的字节数，免得它们撑出一次整块搬迁)
#define MONGO_PACK_BEGIN2(cap, extra) \
    bson_ctx bson; \
    size_t _cap = (size_t)(cap); \
    if (0 != _mongo_cap_toolong(_cap)) { \
        *size = 0; \
        return NULL; \
    } \
    bson_init_prefix(&bson, 0 == _cap ? 0 : _cap + (size_t)(extra), MONGO_MSG_HDR)
// 命令里除大文档外还会拼进去的两段：调用方 options，与事务内的 session options
#define MONGO_OPT_EXTRA(optlens) \
    ((NULL != options ? (optlens) : 0) + ((NULL != mongo->session && NULL != mongo->session->options) ? mongo->session->optionslens : 0))
// 两个 RETURN 共用的通用收尾：$db + 闭合 + 填 OP_MSG 头，bson 的缓冲就是结果，留在 _data（失败时已释放）
#define _MONGO_PACK_TAIL(db) \
        bson_append_utf8(&bson, "$db", (db)); \
        bson_append_end(&bson); \
        void *_data = _mongo_pack_msg(mongo, &bson, size)
// 函数收尾。db 形参为 mongo->db / mongo->authdb 等
#define MONGO_PACK_RETURN(db) do { \
        _MONGO_PACK_TAIL(db); \
        return _data; \
    } while (0)
// 事务内 CRUD 用：事务的第一条命令必须带 startTransaction:true，服务端才真正开启事务。
// 这里只记 _txnstart，started 要等下面那个 RETURN 确认组包成功之后才落——顺序不能颠倒，
// 组包失败时提前消耗掉标志，这条连接上的事务就再也开不起来
#define TRANSACTION_OPTIONS_START \
    int32_t _txnstart = 0; \
    if (NULL != mongo->session) {\
        MONGO_PACK_CAT(mongo->session->options, mongo->session->optionslens);\
        if (0 == mongo->session->started) {\
            bson_append_bool(&bson, "startTransaction", 1);\
            _txnstart = 1;\
        }\
    }
// 同 MONGO_PACK_RETURN，另在组包成功后落 started。_txnstart 由上面那个宏声明，
// 故配错在编译期就报：少了它是未声明标识符，多了它是设了没读
#define MONGO_PACK_RETURN_TXN(db) do { \
        _MONGO_PACK_TAIL(db); \
        if (NULL != _data \
            && 0 != _txnstart) { \
            mongo->session->started = 1; \
        } \
        return _data; \
    } while (0)
// 只带事务上下文(lsid/txnNumber/autocommit)，不带 startTransaction。hello 与 commit/abort 用：
// 前两条按规范不得携带 startTransaction，hello 则根本不是事务命令。
// 取连接当前绑定的 mongo->session；与 commit/abort 的入参 session 必然相等，分叉已由那两个入口挡掉
#define TRANSACTION_OPTIONS \
    if (NULL != mongo->session) {\
        MONGO_PACK_CAT(mongo->session->options, mongo->session->optionslens);\
    }

// 组包前的入参长度闸门：cap 为 0（不预估容量）时无可判，直接放行
static inline int32_t _mongo_cap_toolong(size_t cap) {
    if (cap <= MONGO_MAX_PACK_LENS) {
        return 0;
    }
    LOG_ERROR("mongo document exceeds %d bytes: %zu.", MONGO_MAX_PACK_LENS, cap);
    return 1;
}
// 在 bson 预留的 MONGO_MSG_HDR 字节里就地填 OP_MSG 头（Section kind 0），交出 bson 的缓冲；失败时释放 bson
static void *_mongo_pack_msg(mongo_ctx *mongo, bson_ctx *bson, size_t *size) {
    char *p = bson->doc.data;
    mongo->reqid++;
    *size = bson->doc.offset;
    // 总长在此判:MONGO_PACK_CAT 只管每一片,拼完仍可能超 64MB;而下面要把 *size 写进 4 字节头,
    // 超 4GB 会回绕成一个虚假的小长度。拒法同 MONGO_PACK_CAT:落 ERROR 后返 NULL,由 _mongo_send* 吸收
    if (*size > MONGO_MAX_PACK_LENS) {
        LOG_ERROR("mongo message exceeds %d bytes: %zu.", MONGO_MAX_PACK_LENS, *size);
        BSON_FREE(bson);
        *size = 0;
        return NULL;
    }
    write_le32(p, (uint32_t)*size);
    write_le32(p + 4, (uint32_t)mongo->reqid);
    write_le32(p + 8, 0);
    write_le32(p + 12, OP_MSG);
    write_le32(p + MSG_FLAGS_OFF, (uint32_t)mongo->flags);
    p[MONGO_MSG_HDR - 1] = 0;
    return p;
}
int32_t mongo_pack_check_flag(void *pack, mongo_flags flag) {
    if (NULL == pack) {
        return 0;
    }
    binary_ctx breader;
    binary_init_read(&breader, (char *)pack, MSG_FLAGS_OFF + 4);
    binary_offset(&breader, MSG_FLAGS_OFF);
    return BIT_CHECK((int32_t)binary_get_integer(&breader, 4, 1), flag);
}
void *mongo_pack_scram_client_first(mongo_ctx *mongo, const char *method, size_t *size) {
    *size = 0;
    if (0 == strlen(mongo->authdb)) {
        // authdb 与 db 等长，db 在 mongo_init / mongo_db 已校验过，装得下
        safe_fill_str(mongo->authdb, sizeof(mongo->authdb), mongo->db);
    }
    if (0 == strlen(mongo->user)
        || 0 == strlen(mongo->password)
        || 0 == strlen(mongo->authdb)
        || NULL != mongo->scram) {
        return NULL;
    }
    mongo->scram = scram_init(method, 1);
    if (NULL == mongo->scram) {
        return NULL;
    }
    if (ERR_OK != scram_set_user(mongo->scram, mongo->user, strlen(mongo->user))) {
        scram_free(mongo->scram);
        mongo->scram = NULL;
        return NULL;
    }
    char *first_message = scram_first_message(mongo->scram);
    if (NULL == first_message) {
        scram_free(mongo->scram);
        mongo->scram = NULL;
        return NULL;
    }
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "saslStart", 1);
    bson_append_utf8(&bson, "mechanism", method);
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, first_message, strlen(first_message));
    FREE(first_message);
    bson_append_int32(&bson, "autoAuthorize", 1);
    bson_append_document_begain(&bson, "options");
    bson_append_bool(&bson, "skipEmptyExchange", 1);
    bson_append_end(&bson);//options
    MONGO_PACK_RETURN(mongo->authdb);
}
void *mongo_pack_scram_client_final(mongo_ctx *mongo, int32_t convid, char *client_final, size_t flens, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "saslContinue", 1);
    bson_append_int32(&bson, "conversationId", convid);
    // 长度由调用方给: 本函数不要求 client_final 以 NUL 结尾
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, client_final, flens);
    MONGO_PACK_RETURN(mongo->authdb);
}
void *mongo_pack_hello(mongo_ctx *mongo, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "hello", 1);//不能是事务中的第一项操作
    bson_append_document_begain(&bson, "comment");
    bson_append_utf8(&bson, "application", "srey");
    bson_append_utf8(&bson, "os", OS_NAME);
    bson_append_end(&bson);//comment
    TRANSACTION_OPTIONS
    MONGO_PACK_CAT(options, optlens);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_ping(mongo_ctx *mongo, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "ping", 1);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_drop(mongo_ctx *mongo, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_utf8(&bson, "drop", mongo->collection);
    MONGO_PACK_CAT(options, optlens);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(dlens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "insert", mongo->collection);
    MONGO_PACK_ARR("documents", docs, dlens);
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(ulens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "update", mongo->collection);
    MONGO_PACK_ARR("updates", updates, ulens);
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(dlens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "delete", mongo->collection);
    MONGO_PACK_ARR("deletes", deletes, dlens);
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(olens + nlens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_int32(&bson, "bulkWrite", 1);
    MONGO_PACK_ARR("ops", ops, olens);
    MONGO_PACK_ARR("nsInfo", nsinfo, nlens);
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_find(mongo_ctx *mongo, char *filter, size_t flens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(flens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "find", mongo->collection);
    if (!EMPTYPTR(filter, flens)) {
        bson_append_document(&bson, "filter", filter, flens);
    }
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(pllens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "aggregate", mongo->collection);
    MONGO_PACK_ARR("pipeline", pipeline, pllens);
    const char *cursor = bson_empty(size);
    bson_append_document(&bson, "cursor", (char *)cursor, *size);
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_getmore(mongo_ctx *mongo, int64_t cursorid, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int64(&bson, "getMore", cursorid);//事务外部创建的游标，无法在事务内部调用 getMore
    bson_append_utf8(&bson, "collection", mongo->collection);
    MONGO_PACK_CAT(options, optlens);
    // 用不带 START 的那个：游标是别的命令建出来的，事务真要开也该由那条命令开。
    // 挂 _START 的话，begin 后第一条就是 getMore 时会把 started 消耗在一条服务端必拒的命令上，
    // 真正的首条 CRUD 从此不带 startTransaction，整段事务连环 NoSuchTransaction
    TRANSACTION_OPTIONS
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(cslens + BSON_HEADROOM);
    bson_append_utf8(&bson, "killCursors", mongo->collection);//不能将killCursors 命令指定为ACID 事务中的第一个操作.killCursors 命令，服务器会立即停止指定的游标。它不会等待ACID 事务提交
    MONGO_PACK_ARR("cursors", cursorids, cslens);
    MONGO_PACK_CAT(options, optlens);
    // 同 getMore：上一行注释说的"不能作为事务第一个操作"，靠的就是这里不挂 _START——
    // 否则它自己会去当那个第一操作，还顺手把 started 吃掉
    TRANSACTION_OPTIONS
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(qlens + BSON_HEADROOM);
    bson_append_utf8(&bson, "distinct", mongo->collection);
    bson_append_utf8(&bson, "key", key);
    if (!EMPTYPTR(query, qlens)) {
        bson_append_document(&bson, "query", query, qlens);
    }
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_findandmodify(mongo_ctx *mongo, char *query, size_t qlens, int32_t remove, int32_t pipeline, char *update, size_t ulens,
    char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(qlens + ulens + BSON_HEADROOM);
    bson_append_utf8(&bson, "findAndModify", mongo->collection);
    if (!EMPTYPTR(query, qlens)) {
        bson_append_document(&bson, "query", query, qlens);
    }
    if (remove) {
        bson_append_bool(&bson, "remove", 1);//默认值为 false
    } else if (!EMPTYPTR(update, ulens)) {
        if (pipeline) {
            bson_append_array(&bson, "update", update, ulens);
        } else {
            bson_append_document(&bson, "update", update, ulens);
        }
    }
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_count(mongo_ctx *mongo, char *query, size_t qlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(qlens + BSON_HEADROOM);
    bson_append_utf8(&bson, "count", mongo->collection);
    if (!EMPTYPTR(query, qlens)) {
        bson_append_document(&bson, "query", query, qlens);
    }
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(ilens + BSON_HEADROOM);
    bson_append_utf8(&bson, "createIndexes", mongo->collection);
    MONGO_PACK_ARR("indexes", indexes, ilens);
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db);
}
void *mongo_pack_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(ilens + BSON_HEADROOM);
    bson_append_utf8(&bson, "dropIndexes", mongo->collection);
    MONGO_PACK_ARR("index", indexes, ilens);
    MONGO_PACK_CAT(options, optlens);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_startsession(mongo_ctx *mongo, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "startSession", 1);
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_refreshsession(mongo_session *session, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_array_begain(&bson, "refreshSessions");
    bson_append_document_begain(&bson, "0");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//0
    bson_append_end(&bson);//refreshSessions
    MONGO_PACK_RETURN(mongo->db);
}
void *mongo_pack_endsession(mongo_session *session, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_array_begain(&bson, "endSessions");
    bson_append_document_begain(&bson, "0");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//0
    bson_append_end(&bson);//endSessions
    _MONGO_PACK_TAIL(mongo->db);
    if (NULL != _data) {
        // 包头 flags 恒为 MORETOCOME，覆盖掉照抄来的连接级 flags，连接上的值不动
        write_le32((char *)_data + MSG_FLAGS_OFF, MORETOCOME);
    }
    return _data;
}
char *mongo_transaction_options(mongo_session *session, size_t *lens) {
    // 只吐 doc 不打包消息，故不走 MONGO_PACK_BEGIN：那个宏的容量闸门要写 *size，本函数的出参叫 lens
    bson_ctx bson;
    bson_init(&bson, NULL, 0);
    bson_append_document_begain(&bson, "lsid");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//lsid
    bson_append_int64(&bson, "txnNumber", session->txnnumber);
    bson_append_bool(&bson, "autocommit", 0);
    bson_append_end(&bson);
    *lens = bson.doc.offset;
    return bson.doc.data;
}
// 事务收尾两个 packer 共用：组包从 mongo->session 取事务上下文(TRANSACTION_OPTIONS)，
// 入参 session 必须就是连接当前绑定的那个，分叉了会把这次收尾挂到别人的事务上
static inline int32_t _mongo_txn_bound(mongo_session *session, const char *op) {
    if (session->mongo->session == session) {
        return 1;
    }
    LOG_WARN("mongo connection no longer bound to this session, %s rejected.", op);
    return 0;
}
void *mongo_pack_committransaction(mongo_session *session, char *options, size_t optlens, size_t *size) {
    if (!_mongo_txn_bound(session, "commitTransaction")) {
        *size = 0;
        return NULL;
    }
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "commitTransaction", 1);
    TRANSACTION_OPTIONS
    MONGO_PACK_CAT(options, optlens);
    MONGO_PACK_RETURN(MONGO_TXN_DB);
}
void *mongo_pack_aborttransaction(mongo_session *session, char *options, size_t optlens, size_t *size) {
    if (!_mongo_txn_bound(session, "abortTransaction")) {
        *size = 0;
        return NULL;
    }
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "abortTransaction", 1);
    TRANSACTION_OPTIONS
    MONGO_PACK_CAT(options, optlens);
    MONGO_PACK_RETURN(MONGO_TXN_DB);
}
