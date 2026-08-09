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
    void *pack = mongo_pack_hello(&mongo, NULL, 0, &size);
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
    void *pack = mongo_pack_drop(&mongo, NULL, 0, &size);
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
    void *pack = mongo_pack_insert(&mongo, doc.doc.data, doc.doc.offset, NULL, 0, &size);
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

// mongo_pack_check_flag 读的是包里那份 flagBits，不是连接上的 mongo->flags。
// 组包在锁外做、发送前的加锁又会挂起，期间公开的 set_flag / clear_flag 一改，
// 包里写的和"要不要等回包"的判定就对不上：置位方向让回包没人接、被下一条命令的等待者
// 取走（此后整条连接错开一位），清位方向则去等一个永远不来的回包。
// 两个方向各验一次：组完包再改连接标志，判定必须纹丝不动
static void test_mongo_pack_check_flag(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);

    bson_ctx doc;
    bson_init(&doc, NULL, 0);
    bson_append_document_begain(&doc, "0");
    bson_append_utf8(&doc, "name", "tom");
    bson_append_end(&doc);
    bson_append_end(&doc);

    // 置位 → 组包：包里写着 MORETOCOME
    mongo_set_flag(&mongo, MORETOCOME);
    size_t size = 0;
    void *pack = mongo_pack_insert(&mongo, doc.doc.data, doc.doc.offset, NULL, 0, &size);
    _assert_msg_head(tc, pack, size);
    CuAssertIntEquals(tc, MORETOCOME, _read_le32((char *)pack, _MSG_OFF_FLAGS));
    CuAssertTrue(tc, 0 != mongo_pack_check_flag(pack, MORETOCOME));
    // 别的协程在这条包还没发出去时清了标志，包不受影响
    mongo_clear_flag(&mongo);
    CuAssertIntEquals(tc, 0, mongo_check_flag(&mongo, MORETOCOME));
    CuAssertTrue(tc, 0 != mongo_pack_check_flag(pack, MORETOCOME));
    FREE(pack);

    // 未置位 → 组包：包里没有标志，之后别人置位也不能把它变成"只发不等"
    size = 0;
    pack = mongo_pack_insert(&mongo, doc.doc.data, doc.doc.offset, NULL, 0, &size);
    _assert_msg_head(tc, pack, size);
    CuAssertIntEquals(tc, 0, _read_le32((char *)pack, _MSG_OFF_FLAGS));
    mongo_set_flag(&mongo, MORETOCOME);
    CuAssertTrue(tc, 0 != mongo_check_flag(&mongo, MORETOCOME));
    CuAssertIntEquals(tc, 0, mongo_pack_check_flag(pack, MORETOCOME));
    FREE(pack);

    // NULL 直接返回 0，不落到 binary_init 的内部托管分支去 MALLOC
    CuAssertIntEquals(tc, 0, mongo_pack_check_flag(NULL, MORETOCOME));

    BSON_FREE(&doc);
}

// options 达 MAX_PACK_SIZE 时 bson_cat 整篇丢弃，MONGO_PACK_CAT 令整条命令作废：
// 必须返回 NULL 且把 *size 置 0——旧行为是照常发出缺 options 的命令，服务端返回错误结果集。
// *size 置 0 尤其关键：调用方(coro_utils / lmongo)的 lens/size 是未初始化栈变量
static void test_mongo_pack_oversize_options(CuTest *tc) {
    mongo_ctx mongo;
    char *opts;
    size_t size;
    void *pack;

    _mongo_test_init(&mongo);
    MALLOC(opts, 70000);
    ZERO(opts, 70000);
    opts[0] = (char)0x70;
    opts[1] = (char)0x11;
    opts[2] = (char)0x01;

    size = 12345;
    pack = mongo_pack_insert(&mongo, NULL, 0, opts, 70000, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);

    size = 12345;
    pack = mongo_pack_find(&mongo, NULL, 0, opts, 70000, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);

    size = 12345;
    pack = mongo_pack_drop(&mongo, opts, 70000, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);

    size = 0;
    pack = mongo_pack_drop(&mongo, NULL, 0, &size);
    CuAssertTrue(tc, NULL != pack);
    CuAssertTrue(tc, size > 0);
    FREE(pack);
    FREE(opts);
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
    void *pack = mongo_pack_update(&mongo, arr.doc.data, arr.doc.offset, NULL, 0, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "update"));
    FREE(pack);

    // delete
    pack = mongo_pack_delete(&mongo, arr.doc.data, arr.doc.offset, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "delete"));
    FREE(pack);

    // bulkwrite：bulkWrite:1 而非 utf8
    pack = mongo_pack_bulkwrite(&mongo, arr.doc.data, arr.doc.offset,
                                arr.doc.data, arr.doc.offset, NULL, 0, &size);
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
    void *pack = mongo_pack_find(&mongo, NULL, 0, NULL, 0, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "find"));
    FREE(pack);

    // filter 非 NULL：含 filter 子文档
    bson_ctx f;
    bson_init(&f, NULL, 0);
    bson_append_utf8(&f, "name", "tom");
    bson_append_end(&f);

    pack = mongo_pack_find(&mongo, f.doc.data, f.doc.offset, NULL, 0, &size);
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
    void *pack = mongo_pack_aggregate(&mongo, arr.doc.data, arr.doc.offset, NULL, 0, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "aggregate"));
    FREE(pack);

    // getmore：含 getMore (int64) + collection
    pack = mongo_pack_getmore(&mongo, 0x12345678abcdLL, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "collection"));
    FREE(pack);

    // killcursors：utf8 "killCursors":"<collection>"
    pack = mongo_pack_killcursors(&mongo, arr.doc.data, arr.doc.offset, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "killCursors"));
    FREE(pack);

    // distinct：query=NULL 分支
    pack = mongo_pack_distinct(&mongo, "name", NULL, 0, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "distinct"));
    CuAssertStrEquals(tc, "name",     _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "key"));
    FREE(pack);

    // count：query=NULL
    pack = mongo_pack_count(&mongo, NULL, 0, NULL, 0, &size);
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
                                          1 /*remove*/, 0, NULL, 0, NULL, 0, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll",
        _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "findAndModify"));
    FREE(pack);

    // remove=0, pipeline=0：update 为 document
    pack = mongo_pack_findandmodify(&mongo, body.doc.data, body.doc.offset,
                                    0, 0 /*pipeline*/, body.doc.data, body.doc.offset, NULL, 0, &size);
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
                                    0, 1 /*pipeline*/, body.doc.data, body.doc.offset, NULL, 0, &size);
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
    pack = mongo_pack_findandmodify(&mongo, NULL, 0, 1, 0, NULL, 0, NULL, 0, &size);
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
    void *pack = mongo_pack_createindexes(&mongo, arr.doc.data, arr.doc.offset, NULL, 0, &size);
    char *bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll",
        _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "createIndexes"));
    FREE(pack);

    pack = mongo_pack_dropindexes(&mongo, arr.doc.data, arr.doc.offset, NULL, 0, &size);
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
    size_t topts_lens = 0;
    char *opts = mongo_transaction_options(&session, &topts_lens);
    CuAssertPtrNotNull(tc, opts);
    // BSON 头 4 字节即为文档长度，出参给的缓冲长度须与之一致
    int32_t opts_lens = _read_le32(opts, 0);
    CuAssertTrue(tc, opts_lens > 0);
    CuAssertTrue(tc, topts_lens == (size_t)opts_lens);
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
    pack = mongo_pack_committransaction(&session, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    int32_t err;
    CuAssertTrue(tc, 1.0 == _bson_find_number(bson, size - _MSG_HEAD_LENS, "commitTransaction", &err));
    CuAssertIntEquals(tc, 0, err);
    FREE(pack);

    pack = mongo_pack_aborttransaction(&session, NULL, 0, &size);
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

    // 回归:响应缺 timeoutMinutes 字段时函数须主动把 *timeout 置 0（曾未初始化 → 上层读到不确定值污染会话过期）
    bson_ctx b3;
    bson_init(&b3, NULL, 0);
    bson_append_document_begain(&b3, "id");
    bson_append_binary(&b3, "id", BSON_SUBTYPE_UUID, uuid, UUID_LENS);
    bson_append_end(&b3);
    bson_append_double(&b3, "ok", 1.0);
    bson_append_end(&b3);
    mgopack_ctx mg3;
    ZERO(&mg3, sizeof(mg3));
    mg3.doc = b3.doc.data;
    mg3.dlens = (uint32_t)b3.doc.offset;
    int32_t timeout3 = 999;// 非零哨兵:证明函数主动写 0 而非沿用旧值
    ok = mongo_parse_startsession(&mg3, out_uuid, &timeout3);
    CuAssertIntEquals(tc, 1, ok);
    CuAssertIntEquals(tc, 0, timeout3);
    CuAssertTrue(tc, 0 == memcmp(out_uuid, uuid, UUID_LENS));
    BSON_FREE(&b3);

    bson_ctx b4;
    bson_init(&b4, NULL, 0);
    bson_append_int32(&b4, "timeoutMinutes", 30);
    bson_append_double(&b4, "ok", 1.0);
    bson_append_end(&b4);
    mgopack_ctx mg4;
    ZERO(&mg4, sizeof(mg4));
    mg4.doc = b4.doc.data;
    mg4.dlens = (uint32_t)b4.doc.offset;
    ok = mongo_parse_startsession(&mg4, out_uuid, &timeout);
    CuAssert(tc, "missing id must fail, not return OK leaving uid unwritten (caller would use all-zero UUID as session id)",
        0 == ok);
    BSON_FREE(&b4);
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

// 事务会话的绑定不能跨连接存活,否则 mongo_commit/rollback 那道 mongo->session != session 的
// 守卫形同虚设,会拿旧的 lsid/txnNumber 往新连接上发 commit。但解绑不能放在 _mongo_udfree:
// 它跑在网络线程,而组包侧在属主线程上判 mongo->session 非空后解引用它的 options/started,
// 跨线程置空会让那两步之间读到 NULL。所以断开时只清连接自身的状态,session 留给 mongo_connect
// 在属主线程上解绑(见 coro_utils.c 的 mongo_connect)
static void test_mongo_udfree_keeps_session(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);// 内部 ZERO 过,ref=0 → PROT_REF_RELEASE 短路,不会 FREE 栈上对象
    mongo.sk.fd = (SOCKET)7;
    mongo_session sess;
    ZERO(&sess, sizeof(sess));
    sess.mongo = &mongo;
    sess.started = 1;
    mongo.session = &sess;

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    ud.context = &mongo;
    _mongo_udfree(&ud);

    CuAssertTrue(tc, NULL == ud.context);
    CuAssertTrue(tc, INVALID_SOCK == mongo.sk.fd);
    // session 必须原样留着:改回在这里置空就是把跨线程写又加回来了
    CuAssertTrue(tc, &sess == mongo.session);
    CuAssertTrue(tc, 1 == sess.started);
    // 重复调用安全(context 已置空直接返回)
    _mongo_udfree(&ud);
    CuAssertTrue(tc, &sess == mongo.session);
}
// mongo_user_pwd 的契约是"任一项超长则两个字段都保持原值"。这条不能靠 safe_fill_str
// 边填边判来实现：函数里 secure_zero 排在两次填充之前，一旦折掉前置校验，密码超长的那次
// 调用会留下"新用户名 + 空密码"。单字段的 mongo_db / authdb / collection 没这个问题，
// 靠 safe_fill_str "装不下就不写" 自身即可
static void test_mongo_setter_atomic(CuTest *tc) {
    char toolong[128];
    memset(toolong, 'x', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';

    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    CuAssertStrEquals(tc, "alice", mongo.user);
    CuAssertStrEquals(tc, "secret", mongo.password);

    // 用户名超长：两个字段都不动
    CuAssertIntEquals(tc, ERR_FAILED, mongo_user_pwd(&mongo, toolong, "p2"));
    CuAssertStrEquals(tc, "alice", mongo.user);
    CuAssertStrEquals(tc, "secret", mongo.password);
    // 密码超长：同样两个都不动（折掉前置校验后这里会变成 u2 + 空串）
    CuAssertIntEquals(tc, ERR_FAILED, mongo_user_pwd(&mongo, "u2", toolong));
    CuAssertStrEquals(tc, "alice", mongo.user);
    CuAssertStrEquals(tc, "secret", mongo.password);
    // 合法则两个一起换
    CuAssertIntEquals(tc, ERR_OK, mongo_user_pwd(&mongo, "u2", "p2"));
    CuAssertStrEquals(tc, "u2", mongo.user);
    CuAssertStrEquals(tc, "p2", mongo.password);

    // 单字段 setter：超长不改动原值
    CuAssertIntEquals(tc, ERR_FAILED, mongo_db(&mongo, toolong));
    CuAssertStrEquals(tc, "testdb", mongo.db);
    CuAssertIntEquals(tc, ERR_FAILED, mongo_collection(&mongo, toolong));
    CuAssertStrEquals(tc, "testcoll", mongo.collection);
    // mongo_db 成功时会顺带清掉 collection（切库后旧集合名不再有意义）
    CuAssertIntEquals(tc, ERR_OK, mongo_db(&mongo, "db2"));
    CuAssertStrEquals(tc, "db2", mongo.db);
    CuAssertStrEquals(tc, "", mongo.collection);
    CuAssertIntEquals(tc, ERR_FAILED, mongo_authdb(&mongo, toolong));
    CuAssertIntEquals(tc, ERR_OK, mongo_authdb(&mongo, "admin"));
    CuAssertStrEquals(tc, "admin", mongo.authdb);
}
// getMore / killCursors 不能去当事务的第一个操作：游标是别的命令建出来的，事务真要开也该
// 由那条命令开。这两个原来挂的是 TRANSACTION_OPTIONS_START，begin 之后第一条若是它们，
// 会给自己带上 startTransaction 并把 started 吃掉——这条命令服务端本来就要拒，而真正的首条
// CRUD 从此不再带 startTransaction，整段事务连环 NoSuchTransaction，只能重新 begin
static void test_mongo_pack_getmore_not_start_txn(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    mongo_session session;
    ZERO(&session, sizeof(session));
    session.mongo = &mongo;
    session.txnnumber = 3;
    for (int32_t i = 0; i < UUID_LENS; i++) {
        session.uuid[i] = (char)(i + 1);
    }
    session.options = mongo_transaction_options(&session, &session.optionslens);
    CuAssertPtrNotNull(tc, session.options);
    mongo.session = &session;

    // startTransaction 是 bool 字段，_bson_find_utf8 找不到它，直接在原始 BSON 里找键名
    size_t size = 0;
    size_t idslens = 0;
    const char *ids = bson_empty(&idslens);
    // 事务刚 begin（started=0），第一条发 getMore
    void *pack = mongo_pack_getmore(&mongo, 12345, NULL, 0, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssert(tc, "getMore must not carry startTransaction",
             NULL == memstr(0, pack, size, "startTransaction", strlen("startTransaction")));
    CuAssert(tc, "getMore must not consume the started flag", 0 == session.started);
    // 会话字段仍要带上（lsid/txnNumber/autocommit 来自 session->options）
    CuAssertTrue(tc, NULL != memstr(0, pack, size, "txnNumber", strlen("txnNumber")));
    FREE(pack);

    // killCursors 同理
    pack = mongo_pack_killcursors(&mongo, (char *)ids, idslens, NULL, 0, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssert(tc, "killCursors must not carry startTransaction",
             NULL == memstr(0, pack, size, "startTransaction", strlen("startTransaction")));
    CuAssert(tc, "killCursors must not consume the started flag", 0 == session.started);
    FREE(pack);

    // 真正的首条 CRUD 才带 startTransaction，并在此刻消耗掉 started
    pack = mongo_pack_find(&mongo, NULL, 0, NULL, 0, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, NULL != memstr(0, pack, size, "startTransaction", strlen("startTransaction")));
    CuAssertIntEquals(tc, 1, session.started);
    FREE(pack);
    // 已消耗后第二条 CRUD 不再带
    pack = mongo_pack_find(&mongo, NULL, 0, NULL, 0, &size);
    CuAssertPtrNotNull(tc, pack);
    CuAssertTrue(tc, NULL == memstr(0, pack, size, "startTransaction", strlen("startTransaction")));
    FREE(pack);

    FREE(session.options);
}
void test_mongo_pack(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mongo_pack_ping);
    SUITE_ADD_TEST(suite, test_mongo_pack_hello);
    SUITE_ADD_TEST(suite, test_mongo_pack_drop);
    SUITE_ADD_TEST(suite, test_mongo_pack_insert);
    SUITE_ADD_TEST(suite, test_mongo_pack_check_flag);
    SUITE_ADD_TEST(suite, test_mongo_pack_oversize_options);
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
    SUITE_ADD_TEST(suite, test_mongo_udfree_keeps_session);
    SUITE_ADD_TEST(suite, test_mongo_setter_atomic);
    SUITE_ADD_TEST(suite, test_mongo_pack_getmore_not_start_txn);
}
