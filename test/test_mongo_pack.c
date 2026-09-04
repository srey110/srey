#include "test_mongo_pack.h"
#include "lib.h"
#include "protocol/mongo/mongo_pack.h"
#include "protocol/mongo/mongo_parse.h"
#include "serial/bson.h"

#define _MSG_OFF_KIND   20
#define _MSG_HEAD_LENS  21

// OP_MSG wire 格式偏移：size(4) reqid(4) respto(4) prot(4) flags(4) kind(1) bson...
#define _MSG_OFF_SIZE   0
#define _MSG_OFF_REQID  4
#define _MSG_OFF_RESPTO 8
#define _MSG_OFF_PROT   12
#define _MSG_OFF_FLAGS  16

// 解包入口的 ev / fd / skid 在测试里恒为空：只喂缓冲，不发包也不认连接。
// 三个恒定实参收进薄封装，签名再变时只改这里，不必逐个改调用点
static void *_t_mongo_unpack(int32_t client, buffer_ctx *buf, ud_cxt *ud,
    size_t *size, int32_t *status) {
    return mongo_unpack(NULL, INVALID_SOCK, 0, client, buf, ud, size, status);
}

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

// 在 BSON 顶层查找字段并返回它的 BSON 类型码；未找到返回 -1。
// 用来钉"这个必需字段在、且类型没变"——只查 utf8 命令名的话，
// 删掉 aggregate 的 cursor 子文档、或把 pipeline 从数组写成文档，都看不出来
static int32_t _bson_find_type(char *doc, size_t lens, const char *key) {
    bson_ctx bson;
    bson_init(&bson, doc, lens);
    bson_iter it;
    bson_iter_init(&it, &bson);
    while (bson_iter_next(&it)) {
        if (0 == strcmp(it.key, key)) {
            return (int32_t)it.type;
        }
    }
    return -1;
}

// 把 bson_ctx 已写好的文档挂成一个只读 mgopack_ctx，供 mongo_cursorid / parse_check_error 等吃
static void _mgopack_of(mgopack_ctx *mg, bson_ctx *b) {
    ZERO(mg, sizeof(mgopack_ctx));
    mg->doc = b->doc.data;
    mg->dlens = (uint32_t)b->doc.offset;
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
    CuAssertIntEquals(tc, BSON_DOCUMENT, _bson_find_type(bson, blens, "comment"));
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
    CuAssertIntEquals(tc, BSON_ARRAY, _bson_find_type(bson, blens, "documents"));
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

    // mongo_set_flag 按掩码收 flag：MONGO_FLAGS_ALL 之外的位整个调用作废，
    // 之内的位只有 MORETOCOME 真会置上。这两道判定原来零覆盖（三处调用全传 MORETOCOME），
    // 删掉就会把 CHECKSUM 原样 BIT_SET 进 mongo->flags、再由 _mongo_pack_msg 写进
    // OP_MSG 的 flagBits —— 那是 required bit，而本客户端不附 CRC-32C 尾，
    // 服务端直接断连（同 commit 3df2ed8 记的形态）。lmongo.c 的掩码校验只挡 Lua 入口
    mongo_set_flag(&mongo, CHECKSUM);
    CuAssertTrue(tc, 0 == mongo_check_flag(&mongo, CHECKSUM));
    mongo_set_flag(&mongo, EXHAUSTALLOWED);
    CuAssertTrue(tc, 0 == mongo_check_flag(&mongo, EXHAUSTALLOWED));
    // 组合值：掩码内，MORETOCOME 生效而同行的 EXHAUSTALLOWED 被丢弃。
    // 按单个枚举值全等判定的话这一条会被整个丢掉
    mongo_set_flag(&mongo, MORETOCOME | EXHAUSTALLOWED);
    CuAssertTrue(tc, 0 != mongo_check_flag(&mongo, MORETOCOME));
    CuAssertIntEquals(tc, MORETOCOME, mongo_clear_flag(&mongo));
    // 掩码外的位：整个调用作废，同一次传进来的 MORETOCOME 也不置上
    mongo_set_flag(&mongo, MORETOCOME | 0x04);
    CuAssertIntEquals(tc, 0, mongo_clear_flag(&mongo));
    // 已置上的位不被作废的调用抹掉
    mongo_set_flag(&mongo, MORETOCOME);
    mongo_set_flag(&mongo, 0x04);
    CuAssertIntEquals(tc, MORETOCOME, mongo_clear_flag(&mongo));
    // 0 合法：还原惯用法 set_flag(clear_flag()) 传的可能就是 0
    mongo_set_flag(&mongo, 0);
    CuAssertIntEquals(tc, 0, mongo_clear_flag(&mongo));

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

// options 被拒时 MONGO_PACK_CAT 令整条命令作废：必须返回 NULL 且把 *size 置 0——
// 旧行为是照常发出缺 options 的命令，服务端返回错误结果集。
// *size 置 0 尤其关键：调用方(coro_utils / lmongo)的 lens/size 是未初始化栈变量。
// 拒收只剩两类：bson_cat 的结构性畸形，以及超单包上限 MONGO_MAX_PACK_LENS(64MB)
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

    // 70000 字节的结构合法 options:改前撞 bson_cat 的 64KB 上限被整篇丢弃、整条命令作废,
    // 现在照常拼入 —— 上限归 mongo 层的 MONGO_MAX_PACK_LENS(64MB)。
    // 所有 mongo_pack_* 共用 MONGO_PACK_CAT, 这里用 drop / find 两条代表
    size = 0;
    pack = mongo_pack_drop(&mongo, opts, 70000, &size);
    CuAssertTrue(tc, NULL != pack);
    CuAssertTrue(tc, size > 70000 - 5);
    FREE(pack);

    size = 0;
    pack = mongo_pack_find(&mongo, NULL, 0, opts, 70000, &size);
    CuAssertTrue(tc, NULL != pack);
    CuAssertTrue(tc, size > 70000 - 5);
    FREE(pack);

    // 声明长度超出传入缓冲:bson_cat 的结构性校验照旧拒收,整条命令作废
    size = 12345;
    pack = mongo_pack_drop(&mongo, opts, 5, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);
    FREE(opts);

    // 超单包上限:MONGO_PACK_CAT 先按 lens 挡住、不解引用指针，
    // 拿一个栈字节即可，不必真分配 64MB（同 test_mongo_pack_oversize_docs）
    char oversize = 0;
    size = 12345;
    pack = mongo_pack_drop(&mongo, &oversize, (size_t)MONGO_MAX_PACK_LENS + 1, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);

    size = 0;
    pack = mongo_pack_drop(&mongo, NULL, 0, &size);
    CuAssertTrue(tc, NULL != pack);
    CuAssertTrue(tc, size > 0);
    FREE(pack);
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
    CuAssertIntEquals(tc, BSON_DOCUMENT, _bson_find_type(bson, blens, "filter"));
    FREE(pack);
    BSON_FREE(&f);
}

// mongo_pack_aggregate + getmore + killcursors + distinct + count
static void test_mongo_pack_misc(CuTest *tc) {
    mongo_ctx mongo;
    int32_t err;
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
    // pipeline 与 cursor 都是必需字段：只查命令名的话，删掉 cursor 那两行也照过，
    // 而 mongod 对缺 cursor 的 aggregate 直接回 "The 'cursor' option is required"
    CuAssertIntEquals(tc, BSON_ARRAY, _bson_find_type(bson, size - _MSG_HEAD_LENS, "pipeline"));
    CuAssertIntEquals(tc, BSON_DOCUMENT, _bson_find_type(bson, size - _MSG_HEAD_LENS, "cursor"));
    FREE(pack);

    // getmore：含 getMore (int64) + collection。cursorid 必须原样落进 getMore 字段——
    // 这个值超出 int32 范围，所以"写 0"、"降成 int32 截断"、"键名拼错"三种改法
    // 都会被下面两条断言抓到。mongo_getmore 在 C 侧无业务调用方，组包层是唯一闸门
    pack = mongo_pack_getmore(&mongo, 0x12345678abcdLL, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "collection"));
    err = 1;
    CuAssertDblEquals(tc, 20015998348237.0,
        _bson_find_number(bson, size - _MSG_HEAD_LENS, "getMore", &err), 0);
    CuAssertIntEquals(tc, 0, err);
    FREE(pack);

    // killcursors：utf8 "killCursors":"<collection>" + cursors 数组
    pack = mongo_pack_killcursors(&mongo, arr.doc.data, arr.doc.offset, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "killCursors"));
    CuAssertIntEquals(tc, BSON_ARRAY, _bson_find_type(bson, size - _MSG_HEAD_LENS, "cursors"));
    FREE(pack);

    // distinct：query=NULL 分支
    pack = mongo_pack_distinct(&mongo, "name", NULL, 0, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "distinct"));
    CuAssertStrEquals(tc, "name",     _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "key"));
    CuAssertIntEquals(tc, -1, _bson_find_type(bson, size - _MSG_HEAD_LENS, "query"));// 不该有 query 字段
    FREE(pack);

    // distinct / count 的 query 非空分支：EMPTYPTR 判反的话过滤条件整个丢掉，
    // 服务端拿全集去重 —— 两条命令都返回结果、都不报错，只是答案错
    bson_ctx q;
    bson_init(&q, NULL, 0);
    bson_append_int32(&q, "age", 18);
    bson_append_end(&q);

    pack = mongo_pack_distinct(&mongo, "name", q.doc.data, q.doc.offset, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "distinct"));
    CuAssertIntEquals(tc, BSON_DOCUMENT, _bson_find_type(bson, size - _MSG_HEAD_LENS, "query"));
    FREE(pack);

    // count：query=NULL
    pack = mongo_pack_count(&mongo, NULL, 0, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "count"));
    CuAssertIntEquals(tc, -1, _bson_find_type(bson, size - _MSG_HEAD_LENS, "query"));
    FREE(pack);

    pack = mongo_pack_count(&mongo, q.doc.data, q.doc.offset, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertStrEquals(tc, "testcoll", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "count"));
    CuAssertIntEquals(tc, BSON_DOCUMENT, _bson_find_type(bson, size - _MSG_HEAD_LENS, "query"));
    FREE(pack);
    BSON_FREE(&q);

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
    CuAssertIntEquals(tc, BSON_ARRAY, _bson_find_type(bson, size - _MSG_HEAD_LENS, "update"));
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
    // 每个 find 前重新 bson_init：bson_iter_init 现在自己会把 doc->offset 归零，
    // 这里保留只是让各段互不依赖，不再是必需
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
    CuAssertIntEquals(tc, BSON_ARRAY, _bson_find_type(bson, size - _MSG_HEAD_LENS, "refreshSessions"));
    FREE(pack);

    // endsession
    pack = mongo_pack_endsession(&session, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertIntEquals(tc, BSON_ARRAY, _bson_find_type(bson, size - _MSG_HEAD_LENS, "endSessions"));
    FREE(pack);

    // commit/abort transaction：两个 packer 会比对入参 session 与连接当前绑定的那个，
    // 先建立绑定；不绑定的情形在下面单独验。
    // options 必须先回填：它恒为 NULL 时 lsid/txnNumber 整段不会写进包，
    // 而"事务收尾带得上会话上下文"正是这两条要钉的性质
    session.options = mongo_transaction_options(&session, &session.optionslens);
    CuAssertPtrNotNull(tc, session.options);
    mongo.session = &session;
    pack = mongo_pack_committransaction(&session, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    int32_t err;
    CuAssertTrue(tc, 1.0 == _bson_find_number(bson, size - _MSG_HEAD_LENS, "commitTransaction", &err));
    CuAssertIntEquals(tc, 0, err);
    CuAssertIntEquals(tc, BSON_DOCUMENT, _bson_find_type(bson, size - _MSG_HEAD_LENS, "lsid"));
    CuAssertTrue(tc, NULL != memstr(0, bson, size - _MSG_HEAD_LENS, "txnNumber", strlen("txnNumber")));
    /* 事务收尾一律发往 admin 库 */
    CuAssertStrEquals(tc, "admin", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "$db"));
    FREE(pack);

    pack = mongo_pack_aborttransaction(&session, NULL, 0, &size);
    bson = _assert_msg_head(tc, pack, size);
    CuAssertTrue(tc, 1.0 == _bson_find_number(bson, size - _MSG_HEAD_LENS, "abortTransaction", &err));
    CuAssertIntEquals(tc, 0, err);
    CuAssertIntEquals(tc, BSON_DOCUMENT, _bson_find_type(bson, size - _MSG_HEAD_LENS, "lsid"));
    CuAssertStrEquals(tc, "admin", _bson_find_utf8(bson, size - _MSG_HEAD_LENS, "$db"));
    FREE(pack);
    FREE(session.options);

    // 绑定分叉即拒绝组包：连接指向别的 session（或没绑定）时两个 packer 都返 NULL 并把
    // *size 置 0。不拒的话这次收尾会挂到别人的事务上
    mongo_session other;
    ZERO(&other, sizeof(other));
    other.mongo = &mongo;
    mongo.session = &other;
    size = 123;
    CuAssertPtrEquals(tc, NULL, mongo_pack_committransaction(&session, NULL, 0, &size));
    CuAssertIntEquals(tc, 0, (int)size);
    size = 123;
    CuAssertPtrEquals(tc, NULL, mongo_pack_aborttransaction(&session, NULL, 0, &size));
    CuAssertIntEquals(tc, 0, (int)size);
    mongo.session = NULL;
    size = 123;
    CuAssertPtrEquals(tc, NULL, mongo_pack_committransaction(&session, NULL, 0, &size));
    CuAssertIntEquals(tc, 0, (int)size);
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
    void *pack = mongo_pack_scram_client_final(&mongo, 42, dummy, strlen(dummy), &size);
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
    _mgopack_of(&mg, &b);

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
    _mgopack_of(&mg2, &b2);
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
    _mgopack_of(&mg3, &b3);
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
    _mgopack_of(&mg, &b);
    CuAssertTrue(tc, 0x123456789abcLL == mongo_cursorid(&mg));
    BSON_FREE(&b);

    // 无 cursor 字段 → 0
    bson_ctx b2;
    bson_init(&b2, NULL, 0);
    bson_append_int32(&b2, "ok", 1);
    bson_append_end(&b2);
    mgopack_ctx mg2;
    _mgopack_of(&mg2, &b2);
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
        _mgopack_of(&mg, &b);
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
        _mgopack_of(&mg, &b);
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
        _mgopack_of(&mg, &b);
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
        _mgopack_of(&mg, &b);
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
    _mgopack_of(&mg, &b);
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
    _mgopack_of(&mg2, &b2);
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
    _mgopack_of(&mg3, &b3);
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
    _mgopack_of(&mg4, &b4);
    ok = mongo_parse_startsession(&mg4, out_uuid, &timeout);
    CuAssert(tc, "missing id must fail, not return OK leaving uid unwritten (caller would use all-zero UUID as session id)",
        0 == ok);
    BSON_FREE(&b4);
}

// 合法 kind=0 的 OP_MSG 必须能走通 mongo_unpack。原来全文件唯一那次 mongo_unpack 是
// 拒收用例，四个 mongo_parse_* 都手工填 mgopack_ctx{doc,dlens} 绕过它，于是
// "wire 字节 → doc/dlens"这段换算零测试：算成"跳过 BSON 长度前缀"之类的偏移错误，
// 三个套件照样全过。这里用 mongo_parse_check_error 的返回值反证换算对了——
// doc/dlens 偏一个字节，BSON 就解不出 n=3。
// 后半段验分片重入：mongo 跑在流式 TCP 上，大响应被拆成多次 read 是常态，
// 而 blens < total 那支的 PROT_MOREDATA 也是零覆盖（改成 PROT_ERROR 就是分片即断连）
static void test_mongo_unpack_kind0_ok(CuTest *tc) {
    bson_ctx b;
    bson_init(&b, NULL, 0);
    bson_append_double(&b, "ok", 1.0);
    bson_append_int32(&b, "n", 3);
    bson_append_end(&b);

    size_t blens = b.doc.offset;
    size_t total = 16 + 4 + 1 + blens;
    char *pkt;
    MALLOC(pkt, total);
    char *p = pkt;
    pack_integer(p, (uint64_t)total, 4, 1); p += 4;
    pack_integer(p, 0,      4, 1); p += 4;
    pack_integer(p, 0,      4, 1); p += 4;
    pack_integer(p, OP_MSG, 4, 1); p += 4;
    pack_integer(p, 0,      4, 1); p += 4;// flags
    *p++ = 0;// kind=0：单个 body section
    memcpy(p, b.doc.data, blens);
    BSON_FREE(&b);

    /* 先喂不全：必须报 MOREDATA 而不是 ERROR，且不吐包 */
    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, pkt, total - 1);
    ud_cxt ud;
    ZERO(&ud, sizeof(ud));
    int32_t status = 0;
    void *mgopack = _t_mongo_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrEquals(tc, NULL, mgopack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertTrue(tc, BIT_CHECK(status, PROT_MOREDATA));

    /* 补齐最后一个字节后必须解出来，且 doc/dlens 换算正确 */
    buffer_append(&buf, pkt + total - 1, 1);
    status = 0;
    mgopack = _t_mongo_unpack(0, &buf, &ud, NULL, &status);
    CuAssertPtrNotNull(tc, mgopack);
    CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
    CuAssertIntEquals(tc, 3, mongo_parse_check_error((mgopack_ctx *)mgopack));
    _mongo_pkfree(mgopack);

    FREE(pkt);
    buffer_free(&buf);
}

// mongo_unpack 的 flags 白名单：`0 != (flags & ~MORETOCOME)` → PROT_ERROR。原来唯一那次
// unpack 把 flags 写成 0，既没验 CHECKSUM/EXHAUSTALLOWED 被拒、也没验 MORETOCOME 被放行。
// 删掉那个 if 之后，server 启用 wire checksum 时尾部 4 字节 CRC-32C 会被算进 dlens
// 当成 BSON 的一部分——轻则把成功响应判成失败，重则整条连接从此错位
static void test_mongo_unpack_flags_whitelist(CuTest *tc) {
    static const struct { uint32_t flags; int32_t accept; } cases[] = {
        { 0,              1 },
        { MORETOCOME,     1 },
        { CHECKSUM,       0 },
        { EXHAUSTALLOWED, 0 },
    };
    bson_ctx b;
    buffer_ctx buf;
    ud_cxt ud;
    int32_t status;
    void *mgopack;
    char *pkt;
    char *p;
    size_t blens;
    size_t total;
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bson_init(&b, NULL, 0);
        bson_append_double(&b, "ok", 1.0);
        bson_append_int32(&b, "n", 3);
        bson_append_end(&b);
        blens = b.doc.offset;
        total = 16 + 4 + 1 + blens;
        MALLOC(pkt, total);
        p = pkt;
        pack_integer(p, (uint64_t)total, 4, 1); p += 4;
        pack_integer(p, 0,      4, 1); p += 4;
        pack_integer(p, 0,      4, 1); p += 4;
        pack_integer(p, OP_MSG, 4, 1); p += 4;
        pack_integer(p, cases[i].flags, 4, 1); p += 4;
        *p++ = 0;
        memcpy(p, b.doc.data, blens);
        BSON_FREE(&b);

        buffer_init(&buf);
        buffer_append(&buf, pkt, total);
        ZERO(&ud, sizeof(ud));
        status = 0;
        mgopack = _t_mongo_unpack(0, &buf, &ud, NULL, &status);
        if (cases[i].accept) {
            CuAssertPtrNotNull(tc, mgopack);
            CuAssertTrue(tc, !BIT_CHECK(status, PROT_ERROR));
            _mongo_pkfree(mgopack);
        } else {
            CuAssertPtrEquals(tc, NULL, mgopack);
            CuAssertTrue(tc, BIT_CHECK(status, PROT_ERROR));
        }
        FREE(pkt);
        buffer_free(&buf);
    }
}

// mongo_unpack 拒收 kind=1 + klens=5 + docid='\0' 的语义空 section 响应:
// 协议层合法但 dlens=0,下游 mongo_parse_*/bson_iter_init 触发 ASSERTAB abort,
// 修复后 mongo_unpack 直接 PROT_ERROR 拒收避免恶意 server 26 字节构造响应远程 DoS
static void test_mongo_unpack_kind1_empty_section(CuTest *tc) {
    char pkt[26];
    char *p = pkt;
    pack_integer(p, 26,     4, 1); p += 4;// total size
    pack_integer(p, 0,      4, 1); p += 4;// reqid
    pack_integer(p, 0,      4, 1); p += 4;// respto
    pack_integer(p, OP_MSG, 4, 1); p += 4;// prot
    pack_integer(p, 0,      4, 1); p += 4;// flags
    *p++ = 1;// kind=1
    pack_integer(p, 5,      4, 1); p += 4;// klens=5 (最小合法)
    *p++ = '\0';// docid="\0"
    CuAssertIntEquals(tc, 26, (int)(p - pkt));

    buffer_ctx buf;
    buffer_init(&buf);
    buffer_append(&buf, pkt, sizeof(pkt));

    ud_cxt ud;
    ZERO(&ud, sizeof(ud));

    int32_t status = 0;
    void *mgopack = _t_mongo_unpack(0, &buf, &ud, NULL, &status);
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
// session 不随连接失效：服务端按 lsid 记账、与连接无关，重连只废掉在途事务。
// 代次前进后拿旧 session 重新 begin 是正常用法（txnNumber 递增），只有"连接上已有别的
// session 在事务中"才拒。判定都在网络之前，可纯内存构造
static void test_mongo_session_survives_reconnect(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    mongo.generation = 3;
    mongo_session session;
    ZERO(&session, sizeof(session));
    session.mongo = &mongo;
    for (int32_t i = 0; i < UUID_LENS; i++) {
        session.uuid[i] = (char)(i + 1);
    }

    CuAssertIntEquals(tc, ERR_OK, mongo_begin(&session));
    CuAssertIntEquals(tc, 1, session.txnnumber);
    CuAssertTrue(tc, &session == mongo.session);

    // 重连：代次前进 + 解绑，mongo_connect 内部即这两步
    mongo.generation = 4;
    mongo.session = NULL;
    CuAssertIntEquals(tc, ERR_OK, mongo_begin(&session));
    CuAssertIntEquals(tc, 2, session.txnnumber);
    CuAssertTrue(tc, &session == mongo.session);

    // 绑定门仍在：另一个 session 占着事务时拒绝，且一个字段都不动
    mongo_session other;
    ZERO(&other, sizeof(other));
    other.mongo = &mongo;
    CuAssertIntEquals(tc, ERR_FAILED, mongo_begin(&other));
    CuAssertIntEquals(tc, 0, other.txnnumber);
    CuAssertTrue(tc, NULL == other.options);
    CuAssertTrue(tc, &session == mongo.session);

    FREE(session.options);
}
// 文档数组这类大入参的长度闸门：排在 MONGO_PACK_BEGIN 里、任何分配与拷贝之前，
// 所以指针不会被解引用，用例不必真的准备 64MB 数据
static void test_mongo_pack_oversize_docs(CuTest *tc) {
    mongo_ctx mongo;
    _mongo_test_init(&mongo);
    char dummy = 0;
    size_t size = 12345;

    void *pack = mongo_pack_insert(&mongo, &dummy, (size_t)MONGO_MAX_PACK_LENS + 1, NULL, 0, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);

    size = 12345;
    pack = mongo_pack_update(&mongo, &dummy, (size_t)MONGO_MAX_PACK_LENS + 1, NULL, 0, &size);
    CuAssertTrue(tc, NULL == pack);
    CuAssertTrue(tc, 0 == size);

    // 上限之内照常组包（1 字节的空 bson 文档数组）
    char emptydoc[5] = { 5, 0, 0, 0, 0 };
    size = 0;
    pack = mongo_pack_insert(&mongo, emptydoc, sizeof(emptydoc), NULL, 0, &size);
    CuAssertTrue(tc, NULL != pack);
    CuAssertTrue(tc, size > 0);
    FREE(pack);
}
void test_mongo_pack(CuSuite *suite) {
    SUITE_ADD_TEST(suite, test_mongo_pack_ping);
    SUITE_ADD_TEST(suite, test_mongo_pack_hello);
    SUITE_ADD_TEST(suite, test_mongo_pack_drop);
    SUITE_ADD_TEST(suite, test_mongo_pack_insert);
    SUITE_ADD_TEST(suite, test_mongo_pack_check_flag);
    SUITE_ADD_TEST(suite, test_mongo_pack_oversize_options);
    SUITE_ADD_TEST(suite, test_mongo_pack_oversize_docs);
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
    SUITE_ADD_TEST(suite, test_mongo_unpack_kind0_ok);
    SUITE_ADD_TEST(suite, test_mongo_unpack_flags_whitelist);
    SUITE_ADD_TEST(suite, test_mongo_unpack_kind1_empty_section);
    SUITE_ADD_TEST(suite, test_mongo_udfree_keeps_session);
    SUITE_ADD_TEST(suite, test_mongo_setter_atomic);
    SUITE_ADD_TEST(suite, test_mongo_pack_getmore_not_start_txn);
    SUITE_ADD_TEST(suite, test_mongo_session_survives_reconnect);
}
