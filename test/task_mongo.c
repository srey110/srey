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

    bson_append_end(arr);// close "0"
    bson_append_end(arr);// close array
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
    mongo_drop(mongo, NULL, 0);

    // insert 3 文档
    bson_ctx docs;
    _build_docs_array(&docs);
    int32_t rtn = mongo_insert(mongo, BSON_DOC(&docs), BSON_DOC_LENS(&docs), NULL, 0);
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
    mgopack_ctx *p = mongo_find(mongo, BSON_DOC(&empty), BSON_DOC_LENS(&empty), NULL, 0);
    if (NULL == p) {
        LOG_ERROR("mongo find error.");
        BSON_FREE(&empty);
        return ERR_FAILED;
    }
    BSON_FREE(&empty);

    // count
    bson_ctx empty2;
    _build_empty_doc(&empty2);
    int32_t cnt = mongo_count(mongo, BSON_DOC(&empty2), BSON_DOC_LENS(&empty2), NULL, 0);
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
    rtn = mongo_update(mongo, BSON_DOC(&updates), BSON_DOC_LENS(&updates), NULL, 0);
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
    mongo_drop(mongo, NULL, 0);

    bson_ctx docs;
    bson_init(&docs, NULL, 0);
    bson_append_document_begain(&docs, "0");
    bson_append_int32(&docs, "_id", 999);
    bson_append_utf8(&docs, "name", "first");
    bson_append_end(&docs);
    bson_append_end(&docs);
    int32_t rtn = mongo_insert(mongo, BSON_DOC(&docs), BSON_DOC_LENS(&docs), NULL, 0);
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
    rtn = mongo_insert(mongo, BSON_DOC(&dup), BSON_DOC_LENS(&dup), NULL, 0);
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
    if (ERR_OK != mongo_begin(sess)) {
        LOG_ERROR("mongo begin error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    mongo_collection(mongo, "srey_test");

    bson_ctx docs;
    bson_init(&docs, NULL, 0);
    bson_append_document_begain(&docs, "0");
    bson_append_int32(&docs, "id", 100);
    bson_append_utf8(&docs, "name", "txn-row");
    bson_append_int32(&docs, "score", 1);
    bson_append_end(&docs);
    bson_append_end(&docs);

    int32_t inserted = mongo_insert(mongo, BSON_DOC(&docs), BSON_DOC_LENS(&docs), NULL, 0);
    BSON_FREE(&docs);
    if (1 != inserted) {
        LOG_ERROR("mongo insert(txn) error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    if (ERR_OK != mongo_commit(sess, NULL, 0)) {
        LOG_ERROR("mongo commit error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    mongo_freesession(sess);
    return ERR_OK;
}

// 会话不随连接失效：startsession 拿到 lsid → 断连重连 → 拿旧 session 重新 begin/insert/commit。
// 服务端按 lsid 记账、与连接无关，重连只废掉在途事务。这条钉住的是"不设代次门"这个决定，
// 真跑一遍服务端才算数——纯内存那半在 test_mongo_session_survives_reconnect
static int32_t _txn_reconnect_flow(task_ctx *task, mongo_ctx *mongo) {
    mongo_session *sess = mongo_startsession(mongo);
    if (NULL == sess) {
        LOG_ERROR("mongo startsession(reconnect) error.");
        return ERR_FAILED;
    }
    ev_close(&task->loader->netev, mongo->sk.fd, mongo->sk.skid);
    if (ERR_OK != mongo_ping(mongo)) {
        LOG_ERROR("mongo reconnect(txn) error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    if (ERR_OK != mongo_begin(sess)) {
        LOG_ERROR("mongo begin after reconnect should be accepted.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    mongo_collection(mongo, "srey_test");
    bson_ctx docs;
    bson_init(&docs, NULL, 0);
    bson_append_document_begain(&docs, "0");
    bson_append_int32(&docs, "id", 101);
    bson_append_utf8(&docs, "name", "txn-reconnect");
    bson_append_int32(&docs, "score", 1);
    bson_append_end(&docs);
    bson_append_end(&docs);
    int32_t inserted = mongo_insert(mongo, BSON_DOC(&docs), BSON_DOC_LENS(&docs), NULL, 0);
    BSON_FREE(&docs);
    if (1 != inserted) {
        LOG_ERROR("mongo insert on session across reconnect error, got %d.", inserted);
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    if (ERR_OK != mongo_commit(sess, NULL, 0)) {
        LOG_ERROR("mongo commit on session across reconnect error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    mongo_freesession(sess);
    return ERR_OK;
}
// 事务 pack 失败须原样保留事务状态。用"头里声明的长度超出传入缓冲"的假 options 触发
// bson_cat 拒绝 —— 这是它剩下的结构性拒收之一;字节数上限已归 mongo 层(MONGO_MAX_PACK_LENS
// 64MB),拿 64KB 那种大小再也造不出失败,反而会真把 commit 发出去。
// 本流程不需要 replica set:mongo_begin 纯本地,pack 在 MONGO_PACK_CAT 处失败也不碰网络,
// 判据取 mongo->session 是否仍指向本 session——若守卫仍放在状态拆除之后,它已被置空、
// session->options 已 free,服务端事务会悬到 lsid 超时且无从重试。
// 只压 commit 一侧,rollback 与之同构;mongo_freesession 自己会清 mongo->session
static int32_t _txn_pack_fail_flow(mongo_ctx *mongo) {
    mongo_session *sess = mongo_startsession(mongo);
    if (NULL == sess) {
        LOG_ERROR("mongo startsession(packfail) error.");
        return ERR_FAILED;
    }
    if (ERR_OK != mongo_begin(sess)) {
        LOG_ERROR("mongo begin(packfail) error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    char toolong[64];
    ZERO(toolong, sizeof(toolong));
    // 声明 64KB 但只给 64 字节:bson_cat 的 doclens > lens 那道当场拒
    pack_integer(toolong, 65535, 4, 1);
    int32_t commited = mongo_commit(sess, toolong, sizeof(toolong));
    if (ERR_OK == commited) {
        LOG_ERROR("mongo commit(malformed options) should fail.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    if (mongo->session != sess) {
        LOG_ERROR("mongo commit(pack fail) must keep transaction state.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    mongo_freesession(sess);
    return ERR_OK;
}

// 一条连接同时只允许一个活跃事务：13 个 CRUD 命令的事务上下文都取自 mongo->session,
// 放第二个 session 进来会让后续写静默改跟它走、commit 提交到错的事务上。
// 同 _txn_pack_fail_flow 不需要 replica set：begin 纯本地,被拒时也不发包。
// 三条断言：B 的 begin 被拒 / 被拒时不动已有绑定 / 同一 session 重开事务仍放行
// 两个事务守卫用例共用的开场:开两个 session。任一失败即回滚已开的那个并返回 ERR_FAILED
static int32_t _start_two_sessions(mongo_ctx *mongo, mongo_session **sessa, mongo_session **sessb) {
    *sessa = mongo_startsession(mongo);
    if (NULL == *sessa) {
        LOG_ERROR("mongo startsession(a) error.");
        return ERR_FAILED;
    }
    *sessb = mongo_startsession(mongo);
    if (NULL == *sessb) {
        LOG_ERROR("mongo startsession(b) error.");
        mongo_freesession(*sessa);
        return ERR_FAILED;
    }
    return ERR_OK;
}
static int32_t _txn_second_session_flow(mongo_ctx *mongo) {
    mongo_session *sessa;
    mongo_session *sessb;
    if (ERR_OK != _start_two_sessions(mongo, &sessa, &sessb)) {
        return ERR_FAILED;
    }
    int32_t rtn = ERR_FAILED;
    if (ERR_OK != mongo_begin(sessa)) {
        LOG_ERROR("mongo begin(a) error.");
    } else if (ERR_OK == mongo_begin(sessb)) {
        LOG_ERROR("mongo begin(b) should be rejected while a is active.");
    } else if (mongo->session != sessa) {
        LOG_ERROR("rejected begin must not touch the existing binding.");
    } else if (ERR_OK != mongo_begin(sessa)) {
        LOG_ERROR("same session re-begin must still be allowed.");
    } else {
        rtn = ERR_OK;
    }
    // b 从未绑定,其 freesession 的 mongo->session==session 守卫不会误清 a 的绑定;
    // a 的 freesession 负责把绑定清干净,否则后面的 _txn_flow 会被新守卫拒掉
    mongo_freesession(sessb);
    mongo_freesession(sessa);
    return rtn;
}

// 绑定分叉后 commit/rollback 必须在入口就拒掉,不能把 A 的请求挂到 B 的事务上。
// 分叉在真实环境来自重连:mongo_connect 入口会解绑,这里直接调 mongo_clear_session 造出来。
// 同样不需要 replica set：被拒时不发包,pack 都不做。
// 三条断言：A 的 rollback 被拒 / A 的 commit 被拒 / 两次拒绝都不动 B 的绑定
static int32_t _txn_unbound_flow(mongo_ctx *mongo) {
    mongo_session *sessa;
    mongo_session *sessb;
    if (ERR_OK != _start_two_sessions(mongo, &sessa, &sessb)) {
        return ERR_FAILED;
    }
    int32_t rtn = ERR_FAILED;
    if (ERR_OK != mongo_begin(sessa)) {
        LOG_ERROR("mongo begin(a) error.");
    } else {
        // 模拟重连：绑定被清掉,a 的事务在服务端已随旧连接消失,但 a 这个对象还活着
        mongo_clear_session(mongo);
        if (ERR_OK != mongo_begin(sessb)) {
            LOG_ERROR("begin(b) should be allowed after the binding was cleared.");
        } else if (ERR_OK == mongo_rollback(sessa, NULL, 0)) {
            LOG_ERROR("rollback on an unbound session must be rejected.");
        } else if (mongo->session != sessb) {
            LOG_ERROR("rejected rollback must not touch b's binding.");
        } else if (ERR_OK == mongo_commit(sessa, NULL, 0)) {
            LOG_ERROR("commit on an unbound session must be rejected.");
        } else if (mongo->session != sessb) {
            LOG_ERROR("rejected commit must not touch b's binding.");
        } else {
            rtn = ERR_OK;
        }
    }
    // a 已不是绑定方,其 freesession 守卫不会误清 b;b 的 freesession 负责清干净留给后续用例
    mongo_freesession(sessa);
    mongo_freesession(sessb);
    return rtn;
}

// ping 自动重连（含 re-auth）：强制关闭连接后 mongo_ping 应重连并恢复可用，count 验证
static int32_t _reconnect_flow(task_ctx *task, mongo_ctx *mongo) {
    ev_close(&task->loader->netev, mongo->sk.fd, mongo->sk.skid);
    if (ERR_OK != mongo_ping(mongo)) {
        LOG_ERROR("mongo ping reconnect error.");
        return ERR_FAILED;
    }
    mongo_collection(mongo, "srey_test");
    bson_ctx empty;
    _build_empty_doc(&empty);
    int32_t cnt = mongo_count(mongo, BSON_DOC(&empty), BSON_DOC_LENS(&empty), NULL, 0);
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
    int32_t rtn = mongo_insert(mongo, BSON_DOC(&doc), BSON_DOC_LENS(&doc), NULL, 0);
    BSON_FREE(&doc);
    if (ERR_OK != rtn) {
        mongo_clear_flag(mongo);
        LOG_ERROR("mongo MORETOCOME insert error: %d.", rtn);
        return ERR_FAILED;
    }
    bson_ctx empty;
    _build_empty_doc(&empty);
    int32_t cnt = mongo_count(mongo, BSON_DOC(&empty), BSON_DOC_LENS(&empty), NULL, 0);
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
    // 凭据必须在 connect 之前设好：hello 与认证都由 mongo_connect 内部完成
    mongo_user_pwd(&arg->mongo, arg->user, arg->password);
    mongo_authdb(&arg->mongo, arg->authdb);
    mongo_authmod(&arg->mongo, "SCRAM-SHA-256");
    if (ERR_OK != mongo_connect(task, &arg->mongo)) {
        LOG_ERROR("mongo connect error.");
        return;
    }
    // 连上了：此后任何失败都是真失败，不能再被 optional 白名单吞成 network error
    *(arg->ok) = -1;
    if (ERR_OK != mongo_ping(&arg->mongo)) {
        LOG_ERROR("mongo ping error.");
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
    if (ERR_OK != _crud_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
    if (ERR_OK != _duplicate_key_error(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
    if (ERR_OK != _reconnect_flow(task, &arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
    if (ERR_OK != _moretocome_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
    if (ERR_OK != _txn_pack_fail_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
    if (ERR_OK != _txn_second_session_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
    if (ERR_OK != _txn_unbound_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
    // 事务路径要求 mongo 以副本集运行：docker-compose 的 MONGO_REPLSET 默认 rs0 即满足。
    // 排在全部计数断言之后，故它多插的一行不影响 _crud_flow / _reconnect_flow / _moretocome_flow
    if (ERR_OK != _txn_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
    if (ERR_OK != _txn_reconnect_flow(task, &arg->mongo)) {
        ev_close(&task->loader->netev, arg->mongo.sk.fd, arg->mongo.sk.skid);
        return;
    }
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
