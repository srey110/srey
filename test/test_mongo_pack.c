#include "test_mongo_pack.h"
#include "lib.h"
#include "protocol/mongo/mongo_pack.h"
#include "protocol/mongo/mongo_parse.h"
#include "serial/bson.h"

// OP_MSG wire 格式偏移：size(4) reqid(4) respto(4) prot(4) flags(4) kind(1) bson...
#define _MSG_OFF_SIZE   0
#define _MSG_OFF_REQID  4
#define _MSG_OFF_RESPTO 8
#define _MSG_OFF_PROT   12
#define _MSG_OFF_FLAGS  16
#define _MSG_OFF_KIND   20
#define _MSG_HEAD_LENS  21

// 从 wire 包指定偏移读取小端 int32
static int32_t _read_le32(const char *p, size_t off) {
    const uint8_t *u = (const uint8_t *)p + off;
    return (int32_t)((uint32_t)u[0] | ((uint32_t)u[1] << 8)
                   | ((uint32_t)u[2] << 16) | ((uint32_t)u[3] << 24));
}

// 校验 OP_MSG 包头：size 匹配、prot=2013、kind=0；返回 bson 文档起始指针
static char *_assert_msg_head(CuTest *tc, void *pack, size_t size) {
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, size > _MSG_HEAD_LENS);
    char *p = (char *)pack;
    CuAssertIntEquals(tc, (int)size, _read_le32(p, _MSG_OFF_SIZE));
    CuAssertIntEquals(tc, 0, _read_le32(p, _MSG_OFF_RESPTO));
    CuAssertIntEquals(tc, OP_MSG, _read_le32(p, _MSG_OFF_PROT));
    CuAssertIntEquals(tc, 0, (int)(uint8_t)p[_MSG_OFF_KIND]);
    return p + _MSG_HEAD_LENS;
}

// 初始化一个最小可用的 mongo_ctx（db/collection 名固定，便于在 BSON 中查找）
static void _mongo_test_init(mongo_ctx *mongo) {
    ZERO(mongo, sizeof(*mongo));
    safe_fill_str(mongo->db, sizeof(mongo->db), "testdb");
    safe_fill_str(mongo->collection, sizeof(mongo->collection), "testcoll");
    safe_fill_str(mongo->user, sizeof(mongo->user), "alice");
    safe_fill_str(mongo->password, sizeof(mongo->password), "secret");
}

// 在 BSON 顶层查找 utf8 字段值，未找到返回 NULL
static const char *_bson_find_utf8(char *doc, size_t lens, const char *key) {
    bson_ctx bson;
    bson_init(&bson, doc, lens);
    bson_iter it;
    bson_iter_init(&it, &bson);
    while (bson_iter_next(&it)) {
        if (BSON_UTF8 == it.type && 0 == strcmp(it.key, key)) {
            return bson_iter_utf8(&it, NULL);
        }
    }
    return NULL;
}

// 在 BSON 顶层查找 int32/int64/double 字段，转 double 返回；err 输出 1 = 未找到
static double _bson_find_number(char *doc, size_t lens, const char *key, int32_t *err) {
    *err = 1;
    bson_ctx bson;
    bson_init(&bson, doc, lens);
    bson_iter it;
    bson_iter_init(&it, &bson);
    while (bson_iter_next(&it)) {
        if (0 != strcmp(it.key, key)) {
            continue;
        }
        *err = 0;
        if (BSON_INT32 == it.type) {
            return (double)bson_iter_int32(&it, NULL);
        }
        if (BSON_INT64 == it.type) {
            return (double)bson_iter_int64(&it, NULL);
        }
        if (BSON_DOUBLE == it.type) {
            return bson_iter_double(&it, NULL);
        }
    }
    return 0.0;
}

// mongo_pack_ping 包头 + bson "ping":1 / "$db":"testdb"
static void test_mongo_pack_ping(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    int32_t prev_reqid = mongo.reqid;

    size_t size = 0;
    void *pack = mongo_pack_ping(&mongo, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    // reqid 自增 +1
    CuAssertIntEquals(tc, prev_reqid + 1, _read_le32((char *)pack, _MSG_OFF_REQID));
    CuAssertIntEquals(tc, prev_reqid + 1, mongo.reqid);

    size_t blens = size - _MSG_HEAD_LENS;
    int32_t err;
    double v = _bson_find_number(bson, blens, "ping", &err);
    CuAssertIntEquals(tc, 0, err);
    CuAssertTrue(tc, 1.0 == v);
    CuAssertStrEquals(tc, "testdb", _bson_find_utf8(bson, blens, "$db"));
    FREE(pack);

    // 再 pack 一次：reqid 继续 +1
    pack = mongo_pack_ping(&mongo, &size);
    CuAssertIntEquals(tc, prev_reqid + 2, mongo.reqid);
    FREE(pack);
}

// mongo_pack_hello 应含 "hello":1 + "comment" 子文档 + "$db"
static void test_mongo_pack_hello(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    size_t size = 0;
    void *pack = mongo_pack_hello(&mongo, NULL, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    size_t blens = size - _MSG_HEAD_LENS;
    int32_t err;
    CuAssertTrue(tc, 1.0 == _bson_find_number(bson, blens, "hello", &err));
    CuAssertIntEquals(tc, 0, err);
    CuAssertStrEquals(tc, "testdb", _bson_find_utf8(bson, blens, "$db"));

    // 验证 comment 子文档存在且为 BSON_DOCUMENT 类型
    bson_ctx b;
    bson_init(&b, bson, blens);
    bson_iter it;
    bson_iter_init(&it, &b);
    int32_t comment_found = 0;
    while (bson_iter_next(&it)) {
        if (0 == strcmp(it.key, "comment")) {
            CuAssertIntEquals(tc, BSON_DOCUMENT, (int)it.type);
            comment_found = 1;
            break;
        }
    }
    CuAssertIntEquals(tc, 1, comment_found);
    FREE(pack);
}

// mongo_pack_drop 应含 "drop":"<collection>"
static void test_mongo_pack_drop(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    size_t size = 0;
    void *pack = mongo_pack_drop(&mongo, NULL, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    size_t blens = size - _MSG_HEAD_LENS;
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, blens, "drop"));
    CuAssertStrEquals(tc, "testdb",   _bson_find_utf8(bson, blens, "$db"));
    FREE(pack);
}

// mongo_pack_insert：含 "insert":"<collection>"，且 documents 是 BSON_ARRAY 类型
static void test_mongo_pack_insert(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);

    // 构造 1 个待插入文档：{ name: "tom", age: 18 }
    bson_ctx doc;
    bson_init(&doc, NULL, 0);
    bson_append_document_begain(&doc, "0");
    bson_append_utf8(&doc, "name", "tom");
    bson_append_int32(&doc, "age", 18);
    bson_append_end(&doc);
    bson_append_end(&doc);

    size_t size = 0;
    void *pack = mongo_pack_insert(&mongo, doc.doc.data, doc.doc.offset, NULL, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    size_t blens = size - _MSG_HEAD_LENS;
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, blens, "insert"));

    // documents 字段应为 BSON_ARRAY
    bson_ctx b;
    bson_init(&b, bson, blens);
    bson_iter it;
    bson_iter_init(&it, &b);
    int32_t found = 0;
    while (bson_iter_next(&it)) {
        if (0 == strcmp(it.key, "documents")) {
            CuAssertIntEquals(tc, BSON_ARRAY, (int)it.type);
            found = 1;
            break;
        }
    }
    CuAssertIntEquals(tc, 1, found);
    FREE(pack);
    BSON_FREE(&doc);
}

// mongo_pack_update + delete + bulkwrite：仅校验关键字段名
static void test_mongo_pack_update_delete_bulk(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);

    bson_ctx arr;
    bson_init(&arr, NULL, 0);
    bson_append_document_begain(&arr, "0");
    bson_append_utf8(&arr, "key", "val");
    bson_append_end(&arr);
    bson_append_end(&arr);

    size_t size = 0;
    // update
    void *pack = mongo_pack_update(&mongo, arr.doc.data, arr.doc.offset, NULL, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "update"));
    FREE(pack);

    // delete
    pack = mongo_pack_delete(&mongo, arr.doc.data, arr.doc.offset, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "delete"));
    FREE(pack);

    // bulkwrite：bulkWrite:1 而非 utf8
    pack = mongo_pack_bulkwrite(&mongo, arr.doc.data, arr.doc.offset,
                                arr.doc.data, arr.doc.offset, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    int32_t err;
    CuAssertTrue(tc, 1.0 == _bson_find_number(bson, size - _MSG_HEAD_LENS, "bulkWrite", &err));
    CuAssertIntEquals(tc, 0, err);
    FREE(pack);

    BSON_FREE(&arr);
}

// mongo_pack_find：filter=NULL 与 filter 非 NULL 两个分支
static void test_mongo_pack_find(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    size_t size = 0;

    // filter=NULL：仅含 find 字段
    void *pack = mongo_pack_find(&mongo, NULL, 0, NULL, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "find"));
    FREE(pack);

    // filter 非 NULL：含 filter 子文档
    bson_ctx f;
    bson_init(&f, NULL, 0);
    bson_append_utf8(&f, "name", "tom");
    bson_append_end(&f);

    pack = mongo_pack_find(&mongo, f.doc.data, f.doc.offset, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    size_t blens = size - _MSG_HEAD_LENS;
    bson_ctx b;
    bson_init(&b, bson, blens);
    bson_iter it;
    bson_iter_init(&it, &b);
    int32_t filter_found = 0;
    while (bson_iter_next(&it)) {
        if (0 == strcmp(it.key, "filter")) {
            CuAssertIntEquals(tc, BSON_DOCUMENT, (int)it.type);
            filter_found = 1;
            break;
        }
    }
    CuAssertIntEquals(tc, 1, filter_found);
    FREE(pack);
    BSON_FREE(&f);
}

// mongo_pack_aggregate + getmore + killcursors + distinct + count
static void test_mongo_pack_misc(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);

    bson_ctx arr;
    bson_init(&arr, NULL, 0);
    bson_append_document_begain(&arr, "0");
    bson_append_int32(&arr, "$skip", 5);
    bson_append_end(&arr);
    bson_append_end(&arr);

    size_t size = 0;
    // aggregate：含 aggregate + pipeline array + cursor document
    void *pack = mongo_pack_aggregate(&mongo, arr.doc.data, arr.doc.offset, NULL, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "aggregate"));
    FREE(pack);

    // getmore：含 getMore (int64) + collection
    pack = mongo_pack_getmore(&mongo, 0x12345678abcdLL, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "collection"));
    FREE(pack);

    // killcursors：utf8 "killCursors":"<collection>"
    pack = mongo_pack_killcursors(&mongo, arr.doc.data, arr.doc.offset, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "killCursors"));
    FREE(pack);

    // distinct：query=NULL 分支
    pack = mongo_pack_distinct(&mongo, "name", NULL, 0, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "distinct"));
    CuAssertStrEquals(tc, "name",     _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "key"));
    FREE(pack);

    // count：query=NULL
    pack = mongo_pack_count(&mongo, NULL, 0, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "count"));
    FREE(pack);

    BSON_FREE(&arr);
}

// mongo_pack_findandmodify 4 个分支：remove / update-doc / update-pipeline / query=NULL
static void test_mongo_pack_findandmodify(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);

    bson_ctx body;
    bson_init(&body, NULL, 0);
    bson_append_utf8(&body, "k", "v");
    bson_append_end(&body);

    size_t size = 0;
    // remove=1 分支
    void *pack = mongo_pack_findandmodify(&mongo, body.doc.data, body.doc.offset,
                                          1 /*remove*/, 0, NULL, 0, NULL, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll",
        _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "findAndModify"));
    FREE(pack);

    // remove=0, pipeline=0：update 为 document
    pack = mongo_pack_findandmodify(&mongo, body.doc.data, body.doc.offset,
                                    0, 0 /*pipeline*/, body.doc.data, body.doc.offset, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    bson_ctx b;
    bson_init(&b, bson, size - _MSG_HEAD_LENS);
    bson_iter it;
    bson_iter_init(&it, &b);
    int32_t update_doc = 0;
    while (bson_iter_next(&it)) {
        if (0 == strcmp(it.key, "update")) {
            CuAssertIntEquals(tc, BSON_DOCUMENT, (int)it.type);
            update_doc = 1;
            break;
        }
    }
    CuAssertIntEquals(tc, 1, update_doc);
    FREE(pack);

    // remove=0, pipeline=1：update 为 array
    pack = mongo_pack_findandmodify(&mongo, body.doc.data, body.doc.offset,
                                    0, 1 /*pipeline*/, body.doc.data, body.doc.offset, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    bson_init(&b, bson, size - _MSG_HEAD_LENS);
    bson_iter_init(&it, &b);
    int32_t update_arr = 0;
    while (bson_iter_next(&it)) {
        if (0 == strcmp(it.key, "update")) {
            CuAssertIntEquals(tc, BSON_ARRAY, (int)it.type);
            update_arr = 1;
            break;
        }
    }
    CuAssertIntEquals(tc, 1, update_arr);
    FREE(pack);

    // query=NULL 分支
    pack = mongo_pack_findandmodify(&mongo, NULL, 0, 1, 0, NULL, 0, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll",
        _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "findAndModify"));
    FREE(pack);

    BSON_FREE(&body);
}

// mongo_pack_createindexes / dropindexes
static void test_mongo_pack_indexes(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);

    bson_ctx arr;
    bson_init(&arr, NULL, 0);
    bson_append_document_begain(&arr, "0");
    bson_append_utf8(&arr, "name", "idx_name");
    bson_append_end(&arr);
    bson_append_end(&arr);

    size_t size = 0;
    void *pack = mongo_pack_createindexes(&mongo, arr.doc.data, arr.doc.offset, NULL, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll",
        _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "createIndexes"));
    FREE(pack);

    pack = mongo_pack_dropindexes(&mongo, arr.doc.data, arr.doc.offset, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll",
        _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "dropIndexes"));
    FREE(pack);

    BSON_FREE(&arr);
}

// mongo_pack_startsession：含 "startSession":1
static void test_mongo_pack_startsession(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    size_t size = 0;
    void *pack = mongo_pack_startsession(&mongo, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    int32_t err;
    CuAssertTrue(tc, 1.0 == _bson_find_number(bson, size - _MSG_HEAD_LENS, "startSession", &err));
    CuAssertIntEquals(tc, 0, err);
    FREE(pack);
}

// 测试 session 相关包：refresh/end/committransaction/aborttransaction + transaction_options
static void test_mongo_pack_session(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);

    mongo_session session;
    ZERO(&session, sizeof(session));
    session.mongo = &mongo;
    session.txnnumber = 7;
    // 填一个 UUID（16 字节）
    for (int i = 0; i < UUID_LENS; i++) {
        session.uuid[i] = (char)(i + 1);
    }

    // mongo_transaction_options：含 lsid/txnNumber/autocommit
    char *opts = mongo_transaction_options(&session);
    CuAssertPtrNotNull(tc, opts);
    // BSON 头 4 字节即为文档长度
    int32_t opts_lens = _read_le32(opts, 0);
    CuAssertTrue(tc, opts_lens > 0);
    bson_ctx b;
    bson_iter it;
    bson_iter found;
    // 每个 find 前都重新 bson_init，因为 bson_iter_init 会推进 doc->offset
    bson_init(&b, opts, (size_t)opts_lens);
    bson_iter_init(&it, &b);
    CuAssertIntEquals(tc, ERR_OK, bson_iter_find(&it, "lsid", &found));
    CuAssertIntEquals(tc, BSON_DOCUMENT, (int)found.type);
    bson_init(&b, opts, (size_t)opts_lens);
    bson_iter_init(&it, &b);
    CuAssertIntEquals(tc, ERR_OK, bson_iter_find(&it, "txnNumber", &found));
    CuAssertTrue(tc, 7 == bson_iter_int64(&found, NULL));
    bson_init(&b, opts, (size_t)opts_lens);
    bson_iter_init(&it, &b);
    CuAssertIntEquals(tc, ERR_OK, bson_iter_find(&it, "autocommit", &found));
    // autocommit 字段为 false
    CuAssertIntEquals(tc, 0, bson_iter_bool(&found, NULL));
    FREE(opts);

    size_t size = 0;
    // refreshsession：含 refreshSessions array
    void *pack = mongo_pack_refreshsession(&session, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    bson_init(&b, bson, size - _MSG_HEAD_LENS);
    bson_iter_init(&it, &b);
    int32_t found_refresh = 0;
    while (bson_iter_next(&it)) {
        if (0 == strcmp(it.key, "refreshSessions")) {
            CuAssertIntEquals(tc, BSON_ARRAY, (int)it.type);
            found_refresh = 1;
            break;
        }
    }
    CuAssertIntEquals(tc, 1, found_refresh);
    FREE(pack);

    // endsession
    pack = mongo_pack_endsession(&session, &size);
    bson = _assert_msg_head(tc, pack, size);
    bson_init(&b, bson, size - _MSG_HEAD_LENS);
    bson_iter_init(&it, &b);
    int32_t found_end = 0;
    while (bson_iter_next(&it)) {
        if (0 == strcmp(it.key, "endSessions")) {
            CuAssertIntEquals(tc, BSON_ARRAY, (int)it.type);
            found_end = 1;
            break;
        }
    }
    CuAssertIntEquals(tc, 1, found_end);
    FREE(pack);

    // commit/abort transaction
    pack = mongo_pack_committransaction(&session, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    int32_t err;
    CuAssertTrue(tc, 1.0 == _bson_find_number(bson, size - _MSG_HEAD_LENS, "commitTransaction", &err));
    CuAssertIntEquals(tc, 0, err);
    FREE(pack);

    pack = mongo_pack_aborttransaction(&session, NULL, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertTrue(tc, 1.0 == _bson_find_number(bson, size - _MSG_HEAD_LENS, "abortTransaction", &err));
    CuAssertIntEquals(tc, 0, err);
    FREE(pack);
}

// mongo_pack_scram_client_first：user/password/authdb 三者缺一不可
static void test_mongo_pack_scram_first(CuTest *tc) {
    // 用户名为空 → NULL
    {
        mongo_ctx mongo;
        ZERO(&mongo, sizeof(mongo));
        safe_fill_str(mongo.db, sizeof(mongo.db), "admin");
        safe_fill_str(mongo.password, sizeof(mongo.password), "pwd");
        size_t size = 0;
        void *pack = mongo_pack_scram_client_first(&mongo, "SCRAM-SHA-256", &size);
        CuAssertTrue(tc, NULL == pack);
        // 即使失败，authdb 已被填上 db 的值
        CuAssertStrEquals(tc, "admin", mongo.authdb);
    }
    // 不支持的算法 → scram_init 返回 NULL → 整体返回 NULL
    {
        mongo_ctx mongo;
        _mongo_test_init(&mongo);
        safe_fill_str(mongo.authdb, sizeof(mongo.authdb), "admin");
        size_t size = 0;
        void *pack = mongo_pack_scram_client_first(&mongo, "SCRAM-MD5", &size);
        CuAssertTrue(tc, NULL == pack);
    }
    // 正常路径：scram=NULL → init → 包结构正确
    {
        mongo_ctx mongo;
        _mongo_test_init(&mongo);
        safe_fill_str(mongo.authdb, sizeof(mongo.authdb), "admin");
        size_t size = 0;
        void *pack = mongo_pack_scram_client_first(&mongo, "SCRAM-SHA-256", &size);
        char *bson = _assert_msg_head(tc, pack, size);
        int32_t err;
        // 含 saslStart:1
        CuAssertTrue(tc, 1.0 == _bson_find_number(bson, size - _MSG_HEAD_LENS, "saslStart", &err));
        CuAssertIntEquals(tc, 0, err);
        CuAssertStrEquals(tc, "SCRAM-SHA-256",
            _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "mechanism"));
        CuAssertStrEquals(tc, "admin", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "$db"));
        FREE(pack);
        // mongo->scram 已被初始化
        CuAssertPtrNotNull(tc, mongo.scram);
        // 再次调用：scram 已存在 → 拒绝
        void *pack2 = mongo_pack_scram_client_first(&mongo, "SCRAM-SHA-256", &size);
        CuAssertTrue(tc, NULL == pack2);
        scram_free(mongo.scram);
        mongo.scram = NULL;
    }
}

// mongo_pack_scram_client_final：含 saslContinue + conversationId
static void test_mongo_pack_scram_final(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    safe_fill_str(mongo.authdb, sizeof(mongo.authdb), "admin");
    char dummy[] = "c=biws,r=fakenonce,p=fakeproof";
    size_t size = 0;
    void *pack = mongo_pack_scram_client_final(&mongo, 42, dummy, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    int32_t err;
    CuAssertTrue(tc, 1.0 == _bson_find_number(bson, size - _MSG_HEAD_LENS, "saslContinue", &err));
    CuAssertIntEquals(tc, 0, err);
    double cid = _bson_find_number(bson, size - _MSG_HEAD_LENS, "conversationId", &err);
    CuAssertIntEquals(tc, 0, err);
    CuAssertTrue(tc, 42.0 == cid);
    FREE(pack);
}

// mongo_parse_auth_response：成功 / 失败回包
static void test_mongo_parse_auth_response(CuTest *tc) {
    // 构造一个回包 BSON：{ ok: 1.0, conversationId: 7, done: false, payload: <binary> }
    bson_ctx b;
    bson_init(&b, NULL, 0);
    bson_append_double(&b, "ok", 1.0);
    bson_append_int32(&b, "conversationId", 7);
    bson_append_bool(&b, "done", 0);
    char payload[] = "r=server-nonce,s=salt,i=4096";
    bson_append_binary(&b, "payload", BSON_SUBTYPE_BINARY, payload, sizeof(payload) - 1);
    bson_append_end(&b);

    mgopack_ctx mg;
    ZERO(&mg, sizeof(mg));
    mg.doc = b.doc.data;
    mg.dlens = (uint32_t)b.doc.offset;

    int32_t convid = 0, done = 0;
    char *p = NULL;
    size_t plen = 0;
    int32_t ok = mongo_parse_auth_response(&mg, &convid, &done, &p, &plen);
    CuAssertIntEquals(tc, 1, ok);
    CuAssertIntEquals(tc, 7, convid);
    CuAssertIntEquals(tc, 0, done);
    CuAssertPtrNotNull(tc, p);
    CuAssertIntEquals(tc, (int)(sizeof(payload) - 1), (int)plen);
    BSON_FREE(&b);

    // ok=0 失败回包
    bson_ctx b2;
    bson_init(&b2, NULL, 0);
    bson_append_double(&b2, "ok", 0.0);
    bson_append_end(&b2);
    mgopack_ctx mg2;
    ZERO(&mg2, sizeof(mg2));
    mg2.doc = b2.doc.data;
    mg2.dlens = (uint32_t)b2.doc.offset;
    convid = 999; done = 999;
    p = (char *)1; plen = 999;
    ok = mongo_parse_auth_response(&mg2, &convid, &done, &p, &plen);
    CuAssertIntEquals(tc, 0, ok);
    // 失败时输出参数被复位
    CuAssertIntEquals(tc, 0, convid);
    CuAssertIntEquals(tc, 0, done);
    CuAssertTrue(tc, NULL == p);
    CuAssertIntEquals(tc, 0, (int)plen);
    BSON_FREE(&b2);

    // ok=1 但 payload 缺失 → 返回 0
    bson_ctx b3;
    bson_init(&b3, NULL, 0);
    bson_append_double(&b3, "ok", 1.0);
    bson_append_int32(&b3, "conversationId", 1);
    bson_append_end(&b3);
    mgopack_ctx mg3;
    ZERO(&mg3, sizeof(mg3));
    mg3.doc = b3.doc.data;
    mg3.dlens = (uint32_t)b3.doc.offset;
    ok = mongo_parse_auth_response(&mg3, &convid, &done, &p, &plen);
    CuAssertIntEquals(tc, 0, ok);
    BSON_FREE(&b3);
}

// mongo_cursorid：解析 cursor.id (int64)
static void test_mongo_parse_cursorid(CuTest *tc) {
    // 构造 { cursor: { id: 0x123456789abcLL, firstBatch: [] } }
    bson_ctx b;
    bson_init(&b, NULL, 0);
    bson_append_document_begain(&b, "cursor");
    bson_append_int64(&b, "id", 0x123456789abcLL);
    bson_append_array_begain(&b, "firstBatch");
    bson_append_end(&b);
    bson_append_end(&b);
    bson_append_end(&b);

    mgopack_ctx mg;
    ZERO(&mg, sizeof(mg));
    mg.doc = b.doc.data;
    mg.dlens = (uint32_t)b.doc.offset;
    CuAssertTrue(tc, 0x123456789abcLL == mongo_cursorid(&mg));
    BSON_FREE(&b);

    // 无 cursor 字段 → 0
    bson_ctx b2;
    bson_init(&b2, NULL, 0);
    bson_append_int32(&b2, "ok", 1);
    bson_append_end(&b2);
    mgopack_ctx mg2;
    ZERO(&mg2, sizeof(mg2));
    mg2.doc = b2.doc.data;
    mg2.dlens = (uint32_t)b2.doc.offset;
    CuAssertTrue(tc, 0 == mongo_cursorid(&mg2));
    BSON_FREE(&b2);
}

// mongo_parse_check_error：成功 (ok=1, 无 error 字段) → 返回 n；失败 → ERR_FAILED
static void test_mongo_parse_check_error(CuTest *tc) {
    // 成功：ok=1, n=3 → 返回 3
    {
        bson_ctx b;
        bson_init(&b, NULL, 0);
        bson_append_double(&b, "ok", 1.0);
        bson_append_int32(&b, "n", 3);
        bson_append_end(&b);
        mgopack_ctx mg;
        ZERO(&mg, sizeof(mg));
        mg.doc = b.doc.data;
        mg.dlens = (uint32_t)b.doc.offset;
        CuAssertIntEquals(tc, 3, mongo_parse_check_error(&mg));
        BSON_FREE(&b);
    }
    // ok=0 → 失败
    {
        bson_ctx b;
        bson_init(&b, NULL, 0);
        bson_append_double(&b, "ok", 0.0);
        bson_append_utf8(&b, "errmsg", "auth failed");
        bson_append_end(&b);
        mgopack_ctx mg;
        ZERO(&mg, sizeof(mg));
        mg.doc = b.doc.data;
        mg.dlens = (uint32_t)b.doc.offset;
        CuAssertIntEquals(tc, ERR_FAILED, mongo_parse_check_error(&mg));
        BSON_FREE(&b);
    }
    // ok=1 但 errmsg 存在 → 失败
    {
        bson_ctx b;
        bson_init(&b, NULL, 0);
        bson_append_double(&b, "ok", 1.0);
        bson_append_int32(&b, "n", 0);
        bson_append_utf8(&b, "errmsg", "soft error");
        bson_append_end(&b);
        mgopack_ctx mg;
        ZERO(&mg, sizeof(mg));
        mg.doc = b.doc.data;
        mg.dlens = (uint32_t)b.doc.offset;
        CuAssertIntEquals(tc, ERR_FAILED, mongo_parse_check_error(&mg));
        BSON_FREE(&b);
    }
    // ok=1 含 nErrors=2 → 失败
    {
        bson_ctx b;
        bson_init(&b, NULL, 0);
        bson_append_double(&b, "ok", 1.0);
        bson_append_int32(&b, "nErrors", 2);
        bson_append_end(&b);
        mgopack_ctx mg;
        ZERO(&mg, sizeof(mg));
        mg.doc = b.doc.data;
        mg.dlens = (uint32_t)b.doc.offset;
        CuAssertIntEquals(tc, ERR_FAILED, mongo_parse_check_error(&mg));
        BSON_FREE(&b);
    }
}

// mongo_parse_startsession：含合法 id 子文档 + timeoutMinutes → 成功提取 UUID
static void test_mongo_parse_startsession(CuTest *tc) {
    char uuid[UUID_LENS];
    for (int i = 0; i < UUID_LENS; i++) {
        uuid[i] = (char)(0xa0 + i);
    }
    // 构造 { id: { id: <binary uuid> }, timeoutMinutes: 30, ok: 1.0 }
    bson_ctx b;
    bson_init(&b, NULL, 0);
    bson_append_document_begain(&b, "id");
    bson_append_binary(&b, "id", BSON_SUBTYPE_UUID, uuid, UUID_LENS);
    bson_append_end(&b);
    bson_append_int32(&b, "timeoutMinutes", 30);
    bson_append_double(&b, "ok", 1.0);
    bson_append_end(&b);

    mgopack_ctx mg;
    ZERO(&mg, sizeof(mg));
    mg.doc = b.doc.data;
    mg.dlens = (uint32_t)b.doc.offset;
    char out_uuid[UUID_LENS];
    int32_t timeout = 0;
    int32_t ok = mongo_parse_startsession(&mg, out_uuid, &timeout);
    CuAssertIntEquals(tc, 1, ok);
    CuAssertIntEquals(tc, 30, timeout);
    CuAssertTrue(tc, 0 == memcmp(out_uuid, uuid, UUID_LENS));
    BSON_FREE(&b);

    // ok=0 → 失败
    bson_ctx b2;
    bson_init(&b2, NULL, 0);
    bson_append_double(&b2, "ok", 0.0);
    bson_append_end(&b2);
    mgopack_ctx mg2;
    ZERO(&mg2, sizeof(mg2));
    mg2.doc = b2.doc.data;
    mg2.dlens = (uint32_t)b2.doc.offset;
    ok = mongo_parse_startsession(&mg2, out_uuid, &timeout);
    CuAssertIntEquals(tc, 0, ok);
    BSON_FREE(&b2);
}

// mongo_unpack 拒收 kind=1 + klens=5 + docid='\0' 的语义空 section 响应:
// 协议层合法但 dlens=0,下游 mongo_parse_*/bson_iter_init 触发 ASSERTAB abort,
// 修复后 mongo_unpack 直接 PROT_ERROR 拒收避免恶意 server 26 字节构造响应远程 DoS
static void test_mongo_unpack_kind1_empty_section(CuTest *tc) {
    char pkt[26];
    char *p = pkt;
    pack_integer(p, 26,     4, 1); p += 4;  // total size
    pack_integer(p, 0,      4, 1); p += 4;  // reqid
    pack_integer(p, 0,      4, 1); p += 4;  // respto
    pack_integer(p, OP_MSG, 4, 1); p += 4;  // prot
    pack_integer(p, 0,      4, 1); p += 4;  // flags
    *p++ = 1;                                // kind=1
    pack_integer(p, 5,      4, 1); p += 4;  // klens=5 (最小合法)
    *p++ = '\0';                             // docid="\0"
    CuAssertIntEquals(tc, 26, (int)(p - pkt));

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, pkt, sizeof(pkt));

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));

    int32_t status = 0;
    void *mgopack = mongo_unpack(NULL, &buf, &ud, &status);
    CuAssertPtrEquals(tc, NULL, mgopack);
    CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));

    buffer_free(&buf);
}

void test_mongo_pack(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mongo_pack_ping);
    SUITE_ADD_TEST(suite, test_mongo_pack_hello);
    SUITE_ADD_TEST(suite, test_mongo_pack_drop);
    SUITE_ADD_TEST(suite, test_mongo_pack_insert);
    SUITE_ADD_TEST(suite, test_mongo_pack_update_delete_bulk);
    SUITE_ADD_TEST(suite, test_mongo_pack_find);
    SUITE_ADD_TEST(suite, test_mongo_pack_misc);
    SUITE_ADD_TEST(suite, test_mongo_pack_findandmodify);
    SUITE_ADD_TEST(suite, test_mongo_pack_indexes);
    SUITE_ADD_TEST(suite, test_mongo_pack_startsession);
    SUITE_ADD_TEST(suite, test_mongo_pack_session);
    SUITE_ADD_TEST(suite, test_mongo_pack_scram_first);
    SUITE_ADD_TEST(suite, test_mongo_pack_scram_final);
    SUITE_ADD_TEST(suite, test_mongo_parse_auth_response);
    SUITE_ADD_TEST(suite, test_mongo_parse_cursorid);
    SUITE_ADD_TEST(suite, test_mongo_parse_check_error);
    SUITE_ADD_TEST(suite, test_mongo_parse_startsession);
    SUITE_ADD_TEST(suite, test_mongo_unpack_kind1_empty_section);
}
