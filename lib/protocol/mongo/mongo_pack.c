#include "protocol/mongo/mongo_pack.h"
#include "serial/bson.h"
#include "crypt/scram.h"
#include "protocol/prots_pub.h"

// OP_MSG 头里 flagBits 的字节偏移：size / reqid / respto / opcode 各占 4 字节，见 _mongo_pack_msg
#define MSG_FLAGS_OFF 16
#define MONGO_MSG_HDR 21 // OP_MSG 头：size/reqid/respto/opcode/flags 各 4 字节 + kind 1 字节
// commitTransaction / abortTransaction 按规范只能发往 admin 库，与连接当前的 $db 无关；
// 发错库服务端回 code 13 Unauthorized "may only be run against the admin database"
#define MONGO_TXN_DB "admin"
#define BSON_HEADROOM 256 // 大消息 cap 估算余量（命令名+集合名+元数据+session options 等）
#define MONGO_READ_FLAGS 0 // 要等回包的命令写进包头的 flags：恒不带 MORETOCOME，与连接级 flags 无关
// 组包长度上限：只防 OP_MSG 头的 int32 长度字段装不下。收包上限 MONGO_MAX_PACK_LENS 不管组包，对端上限与本地无关
#define MONGO_PACK_MAX INT32_MAX
// 拼接 options：源文档超组包上限、或结构不合法(bson_cat 拒收)时整条命令作废，不能丢掉 options 接着发。
// *size 显式置 0，早退路径不能留未初始化值。两道拒绝各打各的日志(判据见 bson_cat)
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
// 原样嵌入的子文档 / 数组先过 bson_check_doc，不是恰好占满 lens 的一篇完整 BSON 就拒整条命令，早退同 MONGO_PACK_CAT。
// bson_append_document / array 自己不校验，长度不对会把整篇 BSON 写错位
#define MONGO_PACK_CHECK(name, doc, lens) do { \
        if (ERR_OK != bson_check_doc((doc), (lens))) { \
            LOG_ERROR("mongo %s rejected: malformed bson, %zu bytes.", (name), (size_t)(lens)); \
            *size = 0; \
            BSON_FREE(&bson); \
            return NULL; \
        } \
    } while (0)
// 必填的数组字段，NULL 与空缓冲同样拒
#define MONGO_PACK_ARR(name, doc, lens) do { \
        MONGO_PACK_CHECK(name, doc, lens); \
        bson_append_array(&bson, (name), (doc), (lens)); \
    } while (0)
// 组包三段式：MONGO_PACK_BEGIN 开头，中间按需 MONGO_PACK_CAT 拼外部文档，末尾一个 RETURN。
// RETURN 的 flags 写进包头：写类命令传 mongo->flags（MORETOCOME 可用），要等回包的传 MONGO_READ_FLAGS。
// 中段与收尾必须配对，配错编译期就报（判据见 MONGO_PACK_RETURN_TXN）：
//   事务内 CRUD            TRANSACTION_OPTIONS_START → MONGO_PACK_RETURN_TXN
//   带事务上下文不开事务    TRANSACTION_OPTIONS       → MONGO_PACK_RETURN
//   不涉事务                无                        → MONGO_PACK_RETURN

// 声明并初始化局部 bson_ctx bson，要求函数有名为 size 的出参。
// cap：BSON 预估容量，0=默认；大消息传 dlens + BSON_HEADROOM 消除 doubling 重分配。宏内只求值一次。
// 超组包上限在这里就拒(cap 就是大入参的长度)，要赶在它们被整份拷进缓冲之前。
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
// 两个 RETURN 与 saslStart 组包共用的通用收尾：$db + 闭合 + 填 OP_MSG 头，bson 的缓冲就是结果，留在 _data（失败时已释放）。
// seqs / nseq 是正文之后追加的文档序列(见 mongo_seq)，没有就传 NULL / 0
#define _MONGO_PACK_TAIL_SEQ(db, flags, seqs, nseq) \
        bson_append_utf8(&bson, "$db", (db)); \
        bson_append_end(&bson); \
        void *_data = _mongo_pack_msg(mongo, &bson, (flags), (seqs), (nseq), size)
#define _MONGO_PACK_TAIL(db, flags) _MONGO_PACK_TAIL_SEQ(db, flags, NULL, 0)
// 函数收尾。db 形参为 mongo->db / mongo->authdb 等
#define MONGO_PACK_RETURN(db, flags) do { \
        _MONGO_PACK_TAIL(db, flags); \
        return _data; \
    } while (0)
// 绑着的事务已作废(见 _mongo_txn_stale)时整条命令作废：照发等于在事务已断的连接上接着写
#define MONGO_PACK_TXN_LIVE do { \
        if (0 != _mongo_txn_stale(mongo)) { \
            LOG_WARN("mongo transaction invalidated by reconnect, rollback or begin again."); \
            *size = 0; \
            BSON_FREE(&bson); \
            return NULL; \
        } \
    } while (0)
// 事务内 CRUD 用：事务的第一条命令必须带 startTransaction:true，服务端才真正开启事务。
// 这里只记 _txnstart，started 要等下面那个 RETURN 确认组包成功之后才落——顺序不能颠倒，
// 组包失败时提前消耗掉标志，这条连接上的事务就再也开不起来
#define TRANSACTION_OPTIONS_START \
    int32_t _txnstart = 0; \
    if (NULL != mongo->session) {\
        MONGO_PACK_TXN_LIVE;\
        MONGO_PACK_CAT(mongo->session->options, mongo->session->optionslens);\
        if (0 == mongo->session->started) {\
            bson_append_bool(&bson, "startTransaction", 1);\
            _txnstart = 1;\
        }\
    }
// 同 MONGO_PACK_RETURN，另在组包成功后落 started。_txnstart 由上面那个宏声明，
// 故配错在编译期就报：少了它是未声明标识符，多了它是设了没读
#define MONGO_PACK_RETURN_TXN(db, flags) MONGO_PACK_RETURN_TXN_SEQ(db, flags, NULL, 0)
// 同 MONGO_PACK_RETURN_TXN，正文之后追加文档序列
#define MONGO_PACK_RETURN_TXN_SEQ(db, flags, seqs, nseq) do { \
        _MONGO_PACK_TAIL_SEQ(db, flags, seqs, nseq); \
        if (NULL != _data \
            && 0 != _txnstart) { \
            mongo->session->started = 1; \
        } \
        if (NULL != _data \
            && NULL != mongo->session \
            && 0 != ((flags) & MORETOCOME)) { \
            mongo->session->unacked = 1; \
        } \
        return _data; \
    } while (0)
// 只带事务上下文(lsid/txnNumber/autocommit)，不带 startTransaction。getMore / killCursors 用，理由见各自组包处。
// commit 与 abort 不用它，作废判据各不相同，见各自组包处；哪些命令带事务上下文见 mongo_pack.h 文件头。
// 取连接当前绑定的 mongo->session
#define TRANSACTION_OPTIONS \
    if (NULL != mongo->session) {\
        MONGO_PACK_TXN_LIVE;\
        MONGO_PACK_CAT(mongo->session->options, mongo->session->optionslens);\
    }

// 不嵌进正文、单独按文档序列(OP_MSG Section kind 1)发的数组字段。正文受单文档 16MB 上限约束，
// 序列里只卡每篇文档，整包上限是服务端的 maxMessageSizeBytes(48MB)
typedef struct mongo_seq {
    const char *name;   // 标识符，即命令里这个数组字段的名字
    const char *arr;    // BSON 数组，已过 bson_check_doc
    size_t lens;        // arr 的字节数
}mongo_seq;

// 连接上绑着的事务已随重连作废：CRUD 只看连接换没换，提交交出过也不放行(那只许重发提交，见 mongo_session_stale)
static inline int32_t _mongo_txn_stale(mongo_ctx *mongo) {
    return NULL != mongo->session && mongo->session->skid != mongo->sk.skid;
}
// 组包前的入参长度闸门：cap 为 0（不预估容量）时无可判，直接放行
static inline int32_t _mongo_cap_toolong(size_t cap) {
    if (cap <= MONGO_PACK_MAX) {
        return 0;
    }
    LOG_ERROR("mongo document exceeds %d bytes: %zu.", MONGO_PACK_MAX, cap);
    return 1;
}
// 在已闭合的正文后追加一个 kind 1 段：kind + 段长 + 标识符 + 数组各元素(须都是文档)依次排开。
// 数组只过了外层校验，逐个元素卡长度；有元素不是文档返回 ERR_FAILED
static int32_t _mongo_seq_append(binary_ctx *doc, const mongo_seq *seq) {
    binary_set_int8(doc, 1);
    size_t start = doc->offset;
    binary_set_skip(doc, 4);// 段长，末尾回填
    binary_set_string(doc, seq->name);
    const char *p = seq->arr + 4;
    const char *end = seq->arr + seq->lens - 1;// 末字节是数组的结束符
    const char *nul;
    uint32_t dlens;
    while (p < end) {
        if (BSON_DOCUMENT != (uint8_t)*p) {
            return ERR_FAILED;
        }
        nul = memchr(p + 1, '\0', (size_t)(end - p - 1));// 元素名，数组里是 "0" "1" ...
        if (NULL == nul
            || end - (nul + 1) < 5) {
            return ERR_FAILED;
        }
        p = nul + 1;
        dlens = read_le32(p);
        if (dlens < 5
            || dlens > (size_t)(end - p)) {
            return ERR_FAILED;
        }
        binary_set_binary(doc, p, dlens);
        p += dlens;
    }
    write_le32(doc->data + start, (uint32_t)(doc->offset - start));
    return ERR_OK;
}
// 在 bson 预留的 MONGO_MSG_HDR 字节里就地填 OP_MSG 头（正文是 Section kind 0），seqs 依次追加成 kind 1 段；
// 交出 bson 的缓冲，失败时释放 bson
static void *_mongo_pack_msg(mongo_ctx *mongo, bson_ctx *bson, int32_t flags, const mongo_seq *seqs, int32_t nseq,
                             size_t *size) {
    for (int32_t i = 0; i < nseq; i++) {
        if (ERR_OK != _mongo_seq_append(&bson->doc, &seqs[i])) {
            LOG_ERROR("mongo %s rejected: every element must be a document.", seqs[i].name);
            BSON_FREE(bson);
            *size = 0;
            return NULL;
        }
    }
    char *p = bson->doc.data;
    *size = bson->doc.offset;
    // 总长在此判:MONGO_PACK_CAT 只管每一片,拼完仍可能超出 int32 长度字段。拒法同 MONGO_PACK_CAT:
    // 落 ERROR 后返 NULL,由 _mongo_send* 吸收
    if (*size > MONGO_PACK_MAX) {
        LOG_ERROR("mongo message exceeds %d bytes: %zu.", MONGO_PACK_MAX, *size);
        BSON_FREE(bson);
        *size = 0;
        return NULL;
    }
    uint32_t reqid = (uint32_t)ATOMIC_ADD(&mongo->reqid, 1) + 1;// 本包只用这次取到的号
    write_le32(p, (uint32_t)*size);
    write_le32(p + 4, reqid);
    write_le32(p + 8, 0);
    write_le32(p + 12, OP_MSG);
    write_le32(p + MSG_FLAGS_OFF, (uint32_t)flags);
    p[MONGO_MSG_HDR - 1] = 0;
    return p;
}
int32_t mongo_pack_check_flag(void *pack, mongo_flags flag) {
    if (NULL == pack) {
        return 0;
    }
    return BIT_CHECK((int32_t)read_le32((const char *)pack + MSG_FLAGS_OFF), flag);
}
void *mongo_pack_scram_client_first(mongo_ctx *mongo, const char *method, struct scram_ctx **scram, size_t *size) {
    *size = 0;
    if (0 == strlen(mongo->authdb)) {
        // authdb 与 db 等长，db 在 mongo_init / mongo_db 已校验过，装得下
        safe_fill_str(mongo->authdb, sizeof(mongo->authdb), mongo->db);
    }
    if (0 == strlen(mongo->user)
        || 0 == strlen(mongo->password)
        || 0 == strlen(mongo->authdb)) {
        return NULL;
    }
    scram_ctx *sc = scram_init(method, 1);
    if (NULL == sc) {
        return NULL;
    }
    char *first_message = NULL;
    if (ERR_OK == scram_set_user(sc, mongo->user, strlen(mongo->user))) {
        first_message = scram_first_message(sc);
    }
    if (NULL == first_message) {
        scram_free(sc);
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
    _MONGO_PACK_TAIL(mongo->authdb, MONGO_READ_FLAGS);
    if (NULL == _data) {
        scram_free(sc);
        return NULL;
    }
    *scram = sc;
    return _data;
}
void *mongo_pack_scram_client_final(mongo_ctx *mongo, int32_t convid, char *client_final, size_t flens, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "saslContinue", 1);
    bson_append_int32(&bson, "conversationId", convid);
    // 长度由调用方给: 本函数不要求 client_final 以 NUL 结尾
    bson_append_binary(&bson, "payload", BSON_SUBTYPE_BINARY, client_final, flens);
    MONGO_PACK_RETURN(mongo->authdb, MONGO_READ_FLAGS);
}
void *mongo_pack_hello(mongo_ctx *mongo, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "hello", 1);//不能是事务中的第一项操作
    bson_append_document_begain(&bson, "comment");
    bson_append_utf8(&bson, "application", "srey");
    bson_append_utf8(&bson, "os", OS_NAME);
    bson_append_end(&bson);//comment
    // 恒不带事务上下文：重连握手也走这里，那时可能已有协程在新连接上 begin，带上就是拿 hello 当事务首条，服务端必拒
    MONGO_PACK_CAT(options, optlens);
    MONGO_PACK_RETURN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_ping(mongo_ctx *mongo, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "ping", 1);
    MONGO_PACK_RETURN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_drop(mongo_ctx *mongo, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_utf8(&bson, "drop", mongo->collection);
    MONGO_PACK_CAT(options, optlens);
    MONGO_PACK_RETURN(mongo->db, mongo->flags);
}
void *mongo_pack_insert(mongo_ctx *mongo, char *docs, size_t dlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(dlens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "insert", mongo->collection);
    MONGO_PACK_CHECK("documents", docs, dlens);
    const mongo_seq seq = { "documents", docs, dlens };
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN_SEQ(mongo->db, mongo->flags, &seq, 1);
}
void *mongo_pack_update(mongo_ctx *mongo, char *updates, size_t ulens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(ulens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "update", mongo->collection);
    MONGO_PACK_CHECK("updates", updates, ulens);
    const mongo_seq seq = { "updates", updates, ulens };
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN_SEQ(mongo->db, mongo->flags, &seq, 1);
}
void *mongo_pack_delete(mongo_ctx *mongo, char *deletes, size_t dlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(dlens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "delete", mongo->collection);
    MONGO_PACK_CHECK("deletes", deletes, dlens);
    const mongo_seq seq = { "deletes", deletes, dlens };
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN_SEQ(mongo->db, mongo->flags, &seq, 1);
}
void *mongo_pack_bulkwrite(mongo_ctx *mongo, char *ops, size_t olens, char *nsinfo, size_t nlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(olens + nlens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_int32(&bson, "bulkWrite", 1);
    MONGO_PACK_CHECK("ops", ops, olens);
    MONGO_PACK_CHECK("nsInfo", nsinfo, nlens);
    const mongo_seq seqs[2] = { { "ops", ops, olens }, { "nsInfo", nsinfo, nlens } };
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN_SEQ(mongo->db, mongo->flags, seqs, 2);
}
void *mongo_pack_find(mongo_ctx *mongo, char *filter, size_t flens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(flens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "find", mongo->collection);
    if (!EMPTYPTR(filter, flens)) {
        MONGO_PACK_CHECK("filter", filter, flens);
        bson_append_document(&bson, "filter", filter, flens);
    }
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_aggregate(mongo_ctx *mongo, char *pipeline, size_t pllens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(pllens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "aggregate", mongo->collection);
    MONGO_PACK_ARR("pipeline", pipeline, pllens);
    const char *cursor = bson_empty(size);
    bson_append_document(&bson, "cursor", cursor, *size);
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_getmore(mongo_ctx *mongo, int64_t cursorid, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int64(&bson, "getMore", cursorid);//事务外部创建的游标，无法在事务内部调用 getMore
    bson_append_utf8(&bson, "collection", mongo->collection);
    MONGO_PACK_CAT(options, optlens);
    // 用不带 START 的那个：游标是别的命令建出来的，事务要开也由那条命令开，getMore 不能占掉 startTransaction
    TRANSACTION_OPTIONS
    MONGO_PACK_RETURN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_killcursors(mongo_ctx *mongo, char *cursorids, size_t cslens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(cslens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "killCursors", mongo->collection);//不能将killCursors 命令指定为ACID 事务中的第一个操作.killCursors 命令，服务器会立即停止指定的游标。它不会等待ACID 事务提交
    MONGO_PACK_ARR("cursors", cursorids, cslens);
    MONGO_PACK_CAT(options, optlens);
    // 同 getMore 不挂 _START：它不能当事务的第一个操作
    TRANSACTION_OPTIONS
    MONGO_PACK_RETURN(mongo->db, mongo->flags);
}
void *mongo_pack_distinct(mongo_ctx *mongo, const char *key, char *query, size_t qlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(qlens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "distinct", mongo->collection);
    bson_append_utf8(&bson, "key", key);
    if (!EMPTYPTR(query, qlens)) {
        MONGO_PACK_CHECK("query", query, qlens);
        bson_append_document(&bson, "query", query, qlens);
    }
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_findandmodify(mongo_ctx *mongo, char *query, size_t qlens, int32_t remove, int32_t pipeline, char *update, size_t ulens,
    char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(qlens + ulens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "findAndModify", mongo->collection);
    if (!EMPTYPTR(query, qlens)) {
        MONGO_PACK_CHECK("query", query, qlens);
        bson_append_document(&bson, "query", query, qlens);
    }
    if (remove) {
        bson_append_bool(&bson, "remove", 1);//默认值为 false
    } else if (!EMPTYPTR(update, ulens)) {
        MONGO_PACK_CHECK("update", update, ulens);
        if (pipeline) {
            bson_append_array(&bson, "update", update, ulens);
        } else {
            bson_append_document(&bson, "update", update, ulens);
        }
    }
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_count(mongo_ctx *mongo, char *query, size_t qlens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(qlens + BSON_HEADROOM, NULL != options ? optlens : 0);// 不挂事务上下文，只给 options 留位
    bson_append_utf8(&bson, "count", mongo->collection);
    if (!EMPTYPTR(query, qlens)) {
        MONGO_PACK_CHECK("query", query, qlens);
        bson_append_document(&bson, "query", query, qlens);
    }
    MONGO_PACK_CAT(options, optlens);
    // 服务端不许 count 进事务：带上下文必被拒，还会吃掉 started。绑着事务也按事务外发
    MONGO_PACK_RETURN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_createindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(ilens + BSON_HEADROOM, MONGO_OPT_EXTRA(optlens));
    bson_append_utf8(&bson, "createIndexes", mongo->collection);
    MONGO_PACK_ARR("indexes", indexes, ilens);
    MONGO_PACK_CAT(options, optlens);
    TRANSACTION_OPTIONS_START
    MONGO_PACK_RETURN_TXN(mongo->db, mongo->flags);
}
void *mongo_pack_dropindexes(mongo_ctx *mongo, char *indexes, size_t ilens, char *options, size_t optlens, size_t *size) {
    MONGO_PACK_BEGIN2(ilens + BSON_HEADROOM, NULL != options ? optlens : 0);// 不挂事务上下文，只给 options 留位
    bson_append_utf8(&bson, "dropIndexes", mongo->collection);
    MONGO_PACK_ARR("index", indexes, ilens);
    MONGO_PACK_CAT(options, optlens);
    MONGO_PACK_RETURN(mongo->db, mongo->flags);
}
void *mongo_pack_startsession(mongo_ctx *mongo, size_t *size) {
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "startSession", 1);
    MONGO_PACK_RETURN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_refreshsession(mongo_session *session, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_array_begain(&bson, "refreshSessions");
    bson_append_document_begain(&bson, "0");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//0
    bson_append_end(&bson);//refreshSessions
    MONGO_PACK_RETURN(mongo->db, MONGO_READ_FLAGS);
}
void *mongo_pack_endsession(mongo_session *session, size_t *size) {
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_array_begain(&bson, "endSessions");
    bson_append_document_begain(&bson, "0");
    bson_append_binary(&bson, "id", BSON_SUBTYPE_UUID, session->uuid, UUID_LENS);
    bson_append_end(&bson);//0
    bson_append_end(&bson);//endSessions
    MONGO_PACK_RETURN(mongo->db, MORETOCOME);// 包头恒带 MORETOCOME，只发不等
}
int32_t mongo_session_stale(mongo_session *session) {
    return session->skid != session->mongo->sk.skid
        && 0 == session->committing;
}
uint64_t mongo_txn_mark(mongo_ctx *mongo) {
    return NULL == mongo->session ? MONGO_TXN_NONE : mongo->sk.skid;
}
int32_t mongo_txn_moved(mongo_ctx *mongo, uint64_t mark) {
    return MONGO_TXN_NONE != mark && mark != mongo->sk.skid;
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
// 事务收尾两个 packer 共用：组包从 mongo->session 取事务上下文，
// 入参 session 必须就是连接当前绑定的那个，分叉了会把这次收尾挂到别人的事务上
static inline int32_t _mongo_txn_bound(mongo_session *session, const char *op) {
    if (session->mongo->session != session) {
        LOG_WARN("mongo connection no longer bound to this session, %s rejected.", op);
        return 0;
    }
    return 1;
}
void *mongo_pack_committransaction(mongo_session *session, char *options, size_t optlens, size_t *size) {
    if (!_mongo_txn_bound(session, "commitTransaction")) {
        *size = 0;
        return NULL;
    }
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "commitTransaction", 1);
    // 不走 TRANSACTION_OPTIONS：作废判据要用放行"提交已交出"的那个，网络失败后才能在新连接上重发、由服务端裁决
    if (0 != mongo_session_stale(session)) {
        LOG_WARN("mongo transaction invalidated by reconnect, rollback or begin again.");
        *size = 0;
        BSON_FREE(&bson);
        return NULL;
    }
    MONGO_PACK_CAT(session->options, session->optionslens);
    MONGO_PACK_CAT(options, optlens);
    _MONGO_PACK_TAIL(MONGO_TXN_DB, MONGO_READ_FLAGS);
    // 只有事务内容确定时才许跨重连重发：连接已断(前一条写没等到应答)或发过只发不等的写，都说不清服务端收了哪些
    if (NULL != _data
        && !sock_is_invalid(&mongo->sk)
        && 0 == session->unacked) {
        session->committing = 1;
    }
    return _data;
}
void *mongo_pack_aborttransaction(mongo_session *session, char *options, size_t optlens, size_t *size) {
    if (!_mongo_txn_bound(session, "abortTransaction")) {
        *size = 0;
        return NULL;
    }
    mongo_ctx *mongo = session->mongo;
    MONGO_PACK_BEGIN(0);
    bson_append_int32(&bson, "abortTransaction", 1);
    // 事务已随重连作废也照发：事务挂在会话上而不是连接上，在新连接上中止一样作数，服务端当场放锁
    MONGO_PACK_CAT(session->options, session->optionslens);
    MONGO_PACK_CAT(options, optlens);
    MONGO_PACK_RETURN(MONGO_TXN_DB, MONGO_READ_FLAGS);
}
