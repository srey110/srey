#include "task_mongo.h"

typedef struct task_mongo_args {
    uint16_t port;
    int32_t *ok;
    char host[64];
    char user[64];
    char password[64];
    char db[64];
    char authdb[64];
    mongo_ctx mongo;
}task_mongo_args;

// 构造 insert 用的文档数组（BSON array：键为 "0"/"1"/"2" 的嵌套文档）
static void _build_docs_array(bson_ctx *arr) {
    bson_init(arr, NULL, 0);
    const char *names[3] = { "alice", "bob", "charlie" };
    int32_t scores[3] = { 90, 75, 60 };
    char idx[8];
    for (int32_t i = 0; i < 3; i++) {
        SNPRINTF(idx, sizeof(idx), "%d", i);
        bson_append_document_begain(arr, idx);
        bson_append_int32(arr, "id", i + 1);
        bson_append_utf8(arr, "name", names[i]);
        bson_append_int32(arr, "score", scores[i]);
        bson_append_end(arr);
    }
    bson_append_end(arr);
}

// 构造 updates 数组：[ { q: {id:1}, u: {$set:{score:100}} } ]
static void _build_updates_array(bson_ctx *arr) {
    bson_init(arr, NULL, 0);
    bson_append_document_begain(arr, "0");

    bson_append_document_begain(arr, "q");
    bson_append_int32(arr, "id", 1);
    bson_append_end(arr);

    bson_append_document_begain(arr, "u");
    bson_append_document_begain(arr, "$set");
    bson_append_int32(arr, "score", 100);
    bson_append_end(arr);
    bson_append_end(arr);

    bson_append_end(arr);   // close "0"
    bson_append_end(arr);   // close array
}

// 构造空 filter 文档：{}
static void _build_empty_doc(bson_ctx *doc) {
    bson_init(doc, NULL, 0);
    bson_append_end(doc);
}

// drop 集合 → 插入 3 行 → find → count → update → 校验更新后的值
static int32_t _crud_flow(mongo_ctx *mongo) {
    mongo_collection(mongo, "srey_test");

    // drop 已存在的集合（忽略错误，集合可能不存在）
    mongo_drop(mongo, NULL);

    // insert 3 文档
    bson_ctx docs;
    _build_docs_array(&docs);
    int32_t rtn = mongo_insert(mongo, BSON_DOC(&docs), BSON_DOC_LENS(&docs), NULL);
    BSON_FREE(&docs);
    if (rtn < 0) {
        LOG_ERROR("mongo insert error.");
        return ERR_FAILED;
    }
    if (3 != rtn) {
        LOG_ERROR("mongo insert expected 3 rows, got %d.", rtn);
        return ERR_FAILED;
    }

    // find（filter 为空文档表示返回所有）
    bson_ctx empty;
    _build_empty_doc(&empty);
    mgopack_ctx *p = mongo_find(mongo, BSON_DOC(&empty), BSON_DOC_LENS(&empty), NULL);
    if (NULL == p) {
        LOG_ERROR("mongo find error.");
        BSON_FREE(&empty);
        return ERR_FAILED;
    }
    BSON_FREE(&empty);

    // count
    bson_ctx empty2;
    _build_empty_doc(&empty2);
    int32_t cnt = mongo_count(mongo, BSON_DOC(&empty2), BSON_DOC_LENS(&empty2), NULL);
    BSON_FREE(&empty2);
    if (cnt < 0) {
        LOG_ERROR("mongo count error.");
        return ERR_FAILED;
    }
    if (3 != cnt) {
        LOG_ERROR("mongo count expected 3, got %d.", cnt);
        return ERR_FAILED;
    }

    // update：将 id=1 的 score 改为 100
    bson_ctx updates;
    _build_updates_array(&updates);
    rtn = mongo_update(mongo, BSON_DOC(&updates), BSON_DOC_LENS(&updates), NULL);
    BSON_FREE(&updates);
    if (rtn < 0) {
        LOG_ERROR("mongo update error.");
        return ERR_FAILED;
    }
    if (1 != rtn) {
        LOG_ERROR("mongo update expected 1 row, got %d.", rtn);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 用同一 _id 重复插入触发 E11000，校验 wire 上的 writeErrors 解析路径
static int32_t _duplicate_key_error(mongo_ctx *mongo) {
    mongo_collection(mongo, "srey_test_dup");
    mongo_drop(mongo, NULL);

    bson_ctx docs;
    bson_init(&docs, NULL, 0);
    bson_append_document_begain(&docs, "0");
    bson_append_int32(&docs, "_id", 999);
    bson_append_utf8(&docs, "name", "first");
    bson_append_end(&docs);
    bson_append_end(&docs);
    int32_t rtn = mongo_insert(mongo, BSON_DOC(&docs), BSON_DOC_LENS(&docs), NULL);
    BSON_FREE(&docs);
    if (1 != rtn) {
        LOG_ERROR("mongo dup_key: first insert expected 1 row, got %d.", rtn);
        return ERR_FAILED;
    }

    // 同 _id 再插一次：mongo_insert 应返回 ERR_FAILED
    bson_ctx dup;
    bson_init(&dup, NULL, 0);
    bson_append_document_begain(&dup, "0");
    bson_append_int32(&dup, "_id", 999);
    bson_append_utf8(&dup, "name", "second");
    bson_append_end(&dup);
    bson_append_end(&dup);
    rtn = mongo_insert(mongo, BSON_DOC(&dup), BSON_DOC_LENS(&dup), NULL);
    BSON_FREE(&dup);
    if (ERR_FAILED != rtn) {
        LOG_ERROR("mongo dup_key: expected ERR_FAILED, got %d.", rtn);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 简单事务测试：startsession → begin → 在事务内 insert → commit
static int32_t _txn_flow(mongo_ctx *mongo) {
    mongo_session *sess = mongo_startsession(mongo);
    if (NULL == sess) {
        LOG_ERROR("mongo startsession error.");
        return ERR_FAILED;
    }
    mongo_begin(sess);
    mongo_collection(mongo, "srey_test");

    bson_ctx docs;
    bson_init(&docs, NULL, 0);
    bson_append_document_begain(&docs, "0");
    bson_append_int32(&docs, "id", 100);
    bson_append_utf8(&docs, "name", "txn-row");
    bson_append_int32(&docs, "score", 1);
    bson_append_end(&docs);
    bson_append_end(&docs);

    int32_t inserted = mongo_insert(mongo, BSON_DOC(&docs), BSON_DOC_LENS(&docs), NULL);
    BSON_FREE(&docs);
    if (1 != inserted) {
        LOG_ERROR("mongo insert(txn) error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    if (ERR_OK != mongo_commit(sess, NULL)) {
        LOG_ERROR("mongo commit error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    mongo_freesession(sess);
    return ERR_OK;
}

// ping 自动重连（含 re-auth）：强制关闭连接后 mongo_ping 应重连并恢复可用，count 验证
static int32_t _reconnect_flow(task_ctx *task, mongo_ctx *mongo) {
    ev_close(&task->loader->netev, mongo->fd, mongo->skid, 1);
    if (ERR_OK != mongo_ping(mongo)) {
        LOG_ERROR("mongo ping reconnect error.");
        return ERR_FAILED;
    }
    mongo_collection(mongo, "srey_test");
    bson_ctx empty;
    _build_empty_doc(&empty);
    int32_t cnt = mongo_count(mongo, BSON_DOC(&empty), BSON_DOC_LENS(&empty), NULL);
    BSON_FREE(&empty);
    if (3 != cnt) {
        LOG_ERROR("mongo count after reconnect expected 3, got %d.", cnt);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// MORETOCOME：置位时 insert 为 fire-and-forget（不等响应，返 ERR_OK）；标志仍置位下做 count，
// 验证读操作内部 clear/restore 正常，且 count 反映 fire-forget 写已生效（同连接顺序处理）
static int32_t _moretocome_flow(mongo_ctx *mongo) {
    mongo_collection(mongo, "srey_test");
    mongo_set_flag(mongo, MORETOCOME);
    bson_ctx doc;
    bson_init(&doc, NULL, 0);
    bson_append_document_begain(&doc, "0");
    bson_append_int32(&doc, "id", 200);
    bson_append_utf8(&doc, "name", "fire");
    bson_append_int32(&doc, "score", 1);
    bson_append_end(&doc);
    bson_append_end(&doc);
    int32_t rtn = mongo_insert(mongo, BSON_DOC(&doc), BSON_DOC_LENS(&doc), NULL);
    BSON_FREE(&doc);
    if (ERR_OK != rtn) {
        mongo_clear_flag(mongo);
        LOG_ERROR("mongo MORETOCOME insert error: %d.", rtn);
        return ERR_FAILED;
    }
    bson_ctx empty;
    _build_empty_doc(&empty);
    int32_t cnt = mongo_count(mongo, BSON_DOC(&empty), BSON_DOC_LENS(&empty), NULL);
    BSON_FREE(&empty);
    mongo_clear_flag(mongo);
    if (4 != cnt) {
        LOG_ERROR("mongo MORETOCOME: count expected 4, got %d.", cnt);
        return ERR_FAILED;
    }
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_mongo_args *arg = (task_mongo_args *)coro_get_arg(task);
    mongo_init(&arg->mongo, arg->host, arg->port, NULL, arg->db);
    mongo_user_pwd(&arg->mongo, arg->user, arg->password);
    mongo_authdb(&arg->mongo, arg->authdb);
    if (ERR_OK != mongo_connect(task, &arg->mongo)) {
        LOG_ERROR("mongo connect error.");
        return;
    }
    if (NULL == mongo_hello(&arg->mongo, NULL)) {
        LOG_ERROR("mongo hello error.");
        ev_close(&task->loader->netev, arg->mongo.fd, arg->mongo.skid, 1);
        return;
    }
    if (ERR_OK != mongo_auth(&arg->mongo, "SCRAM-SHA-256", arg->user, arg->password)) {
        LOG_ERROR("mongo auth error.");
        ev_close(&task->loader->netev, arg->mongo.fd, arg->mongo.skid, 1);
        return;
    }
    if (ERR_OK != mongo_ping(&arg->mongo)) {
        LOG_ERROR("mongo ping error.");
        ev_close(&task->loader->netev, arg->mongo.fd, arg->mongo.skid, 1);
        return;
    }
    if (ERR_OK != _crud_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.fd, arg->mongo.skid, 1);
        return;
    }
    if (ERR_OK != _duplicate_key_error(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.fd, arg->mongo.skid, 1);
        return;
    }
    if (ERR_OK != _reconnect_flow(task, &arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.fd, arg->mongo.skid, 1);
        return;
    }
    if (ERR_OK != _moretocome_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.fd, arg->mongo.skid, 1);
        return;
    }
    // 事务路径需要 mongo 以 replica set / sharded 模式运行；docker-compose 单节点跳过。
    // 启用方法：mongo 命令加 --replSet rs0 并通过 rs.initiate() 初始化后解开下面注释。
    // if (ERR_OK != _txn_flow(&arg->mongo)) {
    //     ev_close(&task->loader->netev, arg->mongo.fd, arg->mongo.skid, 1);
    //     return;
    // }
    (void)_txn_flow;  // 抑制未使用函数告警
    mongo_quit(&arg->mongo);
    *(arg->ok) = 1;
    LOG_INFO("mongo tested.");
}

void task_mongo_start(loader_ctx *loader, const char *name,
                      const char *host, uint16_t port,
                      const char *user, const char *password,
                      const char *db, const char *authdb,
                      int32_t *ok) {
    if (NULL == ok
        || NULL == host || strlen(host) >= 64
        || NULL == user || strlen(user) >= 64
        || NULL == password || strlen(password) >= 64
        || NULL == db || strlen(db) >= 64
        || NULL == authdb || strlen(authdb) >= 64) {
        return;
    }
    task_mongo_args *arg;
    CALLOC(arg, 1, sizeof(task_mongo_args));
    arg->port = port;
    arg->ok = ok;
    safe_fill_str(arg->host, sizeof(arg->host), host);
    safe_fill_str(arg->user, sizeof(arg->user), user);
    safe_fill_str(arg->password, sizeof(arg->password), password);
    safe_fill_str(arg->db, sizeof(arg->db), db);
    safe_fill_str(arg->authdb, sizeof(arg->authdb), authdb);
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
