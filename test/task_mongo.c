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
// commit 挂起期间另一个协程重新 begin 用：两个子协程共用，各自回带结果
typedef struct rebegin_arg {
    mongo_session *sess;
    int32_t commit; // 第一次 commit 的结果
    int32_t rtn;    // 挂起期间重新 begin 的结果
}rebegin_arg;
// 组包后、拿到锁前连接被换掉用：一个子协程占锁重连，另一个排在它后面发事务写，各自回带结果
typedef struct moved_arg {
    task_ctx *task;
    mongo_ctx *mongo;
    int32_t reconn; // 占锁那个协程重连的结果
    int32_t insert; // 排队那条事务写的结果
}moved_arg;

static int32_t _count_id(mongo_ctx *mongo, int32_t id);

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
    BSON_FREE(&empty);
    if (NULL == p) {
        LOG_ERROR("mongo find error.");
        return ERR_FAILED;
    }
    // 只判非空的话，写路径把 name/score 整个丢掉都发现不了。
    // 响应形如 { cursor: { firstBatch: [...] }, ok: 1 }，钉住 cursor 在且是子文档
    bson_ctx rsp;
    bson_init(&rsp, p->doc, p->dlens);
    bson_iter it;
    bson_iter_init(&it, &rsp);
    int32_t has_cursor = 0;
    while (bson_iter_next(&it)) {
        if (0 == strcmp(it.key, "cursor")) {
            has_cursor = (BSON_DOCUMENT == it.type);
            break;
        }
    }
    if (!has_cursor) {
        LOG_ERROR("mongo find: response has no cursor document.");
        return ERR_FAILED;
    }

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

// 取响应里 path 指向的数组的元素个数；取不到或不是数组返回 -1
static int32_t _array_count(mgopack_ctx *p, const char *path) {
    bson_ctx rsp;
    bson_init(&rsp, p->doc, p->dlens);
    bson_iter it;
    bson_iter_init(&it, &rsp);
    bson_iter arr;
    if (ERR_OK != bson_iter_find(&it, path, &arr)
        || BSON_ARRAY != arr.type) {
        return -1;
    }
    bson_ctx sub;
    bson_init(&sub, arr.val, arr.lens);
    bson_iter_init(&it, &sub);
    int32_t n = 0;
    while (bson_iter_next(&it)) {
        n++;
    }
    return n;
}
// 游标分批取：find 带 batchSize=1 只回首条，getMore 逐批取到游标号归零，条数须等于 _crud_flow 插的 3 条；
// 再开一个游标用 killCursors 关掉，回包的 cursorsKilled 里须正好一个
static int32_t _cursor_flow(mongo_ctx *mongo) {
    mongo_collection(mongo, "srey_test");
    bson_ctx opts;
    bson_init(&opts, NULL, 0);
    bson_append_int32(&opts, "batchSize", 1);
    bson_append_end(&opts);
    bson_ctx empty;
    _build_empty_doc(&empty);
    int32_t rtn = ERR_FAILED;
    // 回包只到本协程下次挂起前有效：条数与游标号都要在发下一条命令前取出来
    mgopack_ctx *p = mongo_find(mongo, BSON_DOC(&empty), BSON_DOC_LENS(&empty), BSON_DOC(&opts), BSON_DOC_LENS(&opts));
    int32_t got = (NULL == p) ? -1 : _array_count(p, "cursor.firstBatch");
    int64_t cid = (NULL == p) ? 0 : mongo_cursorid(p);
    int32_t n;
    bson_ctx ids;// 声明在 goto 之前，跳到 done 不越过它
    while (got > 0 && 0 != cid) {
        p = mongo_getmore(mongo, cid, BSON_DOC(&opts), BSON_DOC_LENS(&opts));
        n = (NULL == p) ? -1 : _array_count(p, "cursor.nextBatch");
        if (n < 0) {
            got = -1;
            break;
        }
        got += n;
        cid = mongo_cursorid(p);
    }
    if (3 != got) {
        LOG_ERROR("mongo getMore: expected 3 documents across batches, got %d.", got);
        goto done;
    }
    p = mongo_find(mongo, BSON_DOC(&empty), BSON_DOC_LENS(&empty), BSON_DOC(&opts), BSON_DOC_LENS(&opts));
    cid = (NULL == p) ? 0 : mongo_cursorid(p);
    if (0 == cid) {
        LOG_ERROR("mongo killCursors: find with batchSize 1 left no open cursor.");
        goto done;
    }
    bson_init(&ids, NULL, 0);
    bson_append_int64(&ids, "0", cid);
    bson_append_end(&ids);
    p = mongo_killcursors(mongo, BSON_DOC(&ids), BSON_DOC_LENS(&ids), NULL, 0);
    BSON_FREE(&ids);
    if (NULL == p
        || 1 != _array_count(p, "cursorsKilled")) {
        LOG_ERROR("mongo killCursors: the cursor was not reported as killed.");
        goto done;
    }
    rtn = ERR_OK;
done:
    BSON_FREE(&empty);
    BSON_FREE(&opts);
    return rtn;
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
    // 断连也会返 ERR_FAILED：再发一条命令确认连接还活着，否则这条断言分不出
    // "服务端按 E11000 拒了" 与 "连接掉了"
    if (ERR_OK != mongo_ping(mongo)) {
        LOG_ERROR("mongo dup_key: connection died instead of being rejected.");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 简单事务测试：startsession(本地生成 lsid) → begin → 在事务内 insert → commit → freesession，
// 再跟一条普通 insert：endSessions 只发不等，服务端若回了包，这条会读到它而拿不到 n=1
static int32_t _txn_flow(mongo_ctx *mongo) {
    mongo_session *sess = mongo_startsession(mongo);
    if (NULL == sess) {
        LOG_ERROR("mongo startsession error.");
        return ERR_FAILED;
    }
    // 超时分钟数来自建连时 hello 回的 logicalSessionTimeoutMinutes，取不到就是解析断了
    if (sess->timeoutmin <= 0 || sess->timeoutmin != mongo->sesstimeout) {
        LOG_ERROR("mongo session timeout not taken from hello: %d / %d.", sess->timeoutmin, mongo->sesstimeout);
        mongo_freesession(sess);
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
    // count 不进事务(见 mongo_pack_count)：绑着事务照样能发、看不到还没提交的这条，也不把事务弄废，下面 commit 照常
    if (0 != _count_id(mongo, 100)) {
        LOG_ERROR("mongo count inside a bound transaction should run outside it and see 0.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    if (ERR_OK != mongo_commit(sess, NULL, 0)) {
        LOG_ERROR("mongo commit error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    mongo_freesession(sess);
    bson_init(&docs, NULL, 0);
    bson_append_document_begain(&docs, "0");
    bson_append_int32(&docs, "id", 102);
    bson_append_utf8(&docs, "name", "after-endsessions");
    bson_append_int32(&docs, "score", 1);
    bson_append_end(&docs);
    bson_append_end(&docs);
    inserted = mongo_insert(mongo, BSON_DOC(&docs), BSON_DOC_LENS(&docs), NULL, 0);
    BSON_FREE(&docs);
    if (1 != inserted) {
        LOG_ERROR("mongo insert after freesession expected 1, got %d (endSessions replied?).", inserted);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 会话不随连接失效：startsession 拿到 lsid → refresh 让服务端在旧连接上登记会话 → 断连重连 →
// 拿旧 session 重新 begin/insert/commit。服务端按 lsid 记账、与连接无关，重连只废掉在途事务。
// 这条钉住的是"不设代次门"这个决定，真跑一遍服务端才算数——纯内存那半在 test_mongo_session_survives_reconnect
static int32_t _txn_reconnect_flow(task_ctx *task, mongo_ctx *mongo) {
    mongo_session *sess = mongo_startsession(mongo);
    if (NULL == sess) {
        LOG_ERROR("mongo startsession(reconnect) error.");
        return ERR_FAILED;
    }
    // lsid 是本地生成的，不先发一条带它的命令，服务端要到新连接上才第一次见到它
    if (ERR_OK != mongo_refreshsession(sess)) {
        LOG_ERROR("mongo refreshsession(reconnect) error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    uint64_t oldskid = mongo->sk.skid;
    ev_close(&task->loader->netev, &mongo->sk);
    if (ERR_OK != mongo_ping(mongo)) {
        LOG_ERROR("mongo reconnect(txn) error.");
        mongo_freesession(sess);
        return ERR_FAILED;
    }
    if (oldskid == mongo->sk.skid) {
        LOG_ERROR("mongo reconnect(txn): skid unchanged, no reconnect happened.");
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
// bson_cat 拒绝 —— 这是它剩下的结构性拒收之一;字节数上限已归 mongo 组包层(INT32_MAX),
// 拿 64KB 那种大小再也造不出失败,反而会真把 commit 发出去。
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
    // 装成首条命令已组出过：空事务的 commit 不组包、本地直接按成功收尾，就测不到组包失败了
    sess->started = 1;
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
// 重连不再解绑(只让事务作废,见 _txn_stale_flow),分叉只剩手动 mongo_clear_session 这一个来源,这里直接调它造出来。
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
        // 手动清掉绑定：a 这个对象还活着,但连接已不认它
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

// 空事务：begin 之后一条命令都没组出就 commit / rollback，不发包、本地解绑并按成功返回。
// 真发出去的话服务端不认识这个 txnNumber(副本集回 NoSuchTransaction，单机直接拒事务号)，结果必是失败，
// 故返回 ERR_OK 本身就说明没发。begin 后断连重连、事务已作废的空事务 commit 同样本地收尾。不需要 replica set
static int32_t _txn_empty_flow(task_ctx *task, mongo_ctx *mongo) {
    mongo_session *sess = mongo_startsession(mongo);
    if (NULL == sess) {
        LOG_ERROR("mongo startsession(empty) error.");
        return ERR_FAILED;
    }
    int32_t rtn = ERR_FAILED;
    if (ERR_OK != mongo_begin(sess)
        || ERR_OK != mongo_commit(sess, NULL, 0)) {
        LOG_ERROR("mongo empty transaction commit should succeed locally.");
    } else if (NULL != mongo->session
        || NULL != sess->options) {
        LOG_ERROR("mongo empty transaction commit must unbind and free the options.");
    } else if (ERR_OK != mongo_begin(sess)
        || ERR_OK != mongo_rollback(sess, NULL, 0)
        || NULL != mongo->session) {
        LOG_ERROR("mongo empty transaction rollback should succeed locally and unbind.");
    } else if (ERR_OK != mongo_begin(sess)) {
        LOG_ERROR("mongo begin(empty, stale) error.");
    } else {
        uint64_t oldskid = mongo->sk.skid;
        ev_close(&task->loader->netev, &mongo->sk);
        if (ERR_OK != mongo_ping(mongo)
            || oldskid == mongo->sk.skid) {
            LOG_ERROR("mongo empty transaction: ping did not reconnect.");
        } else if (ERR_OK != mongo_commit(sess, NULL, 0)
            || NULL != mongo->session) {
            LOG_ERROR("mongo stale empty transaction commit should succeed locally and unbind.");
        } else {
            rtn = ERR_OK;
        }
    }
    mongo_freesession(sess);
    return rtn;
}

// 按 id 数文档条数；失败返回负数
static int32_t _count_id(mongo_ctx *mongo, int32_t id) {
    bson_ctx filter;
    bson_init(&filter, NULL, 0);
    bson_append_int32(&filter, "id", id);
    bson_append_end(&filter);
    int32_t cnt = mongo_count(mongo, BSON_DOC(&filter), BSON_DOC_LENS(&filter), NULL, 0);
    BSON_FREE(&filter);
    return cnt;
}
// 插一条 {id, name: tag}；返回 mongo_insert 的结果
static int32_t _insert_id(mongo_ctx *mongo, int32_t id, const char *tag) {
    bson_ctx docs;
    bson_init(&docs, NULL, 0);
    bson_append_document_begain(&docs, "0");
    bson_append_int32(&docs, "id", id);
    bson_append_utf8(&docs, "name", tag);
    bson_append_end(&docs);
    bson_append_end(&docs);
    int32_t rtn = mongo_insert(mongo, BSON_DOC(&docs), BSON_DOC_LENS(&docs), NULL, 0);
    BSON_FREE(&docs);
    return rtn;
}
// 事务中途断连：begin → insert(在事务里) → 断连 → ping 重连 → 同一事务的写必须失败，不能脱离事务
// 悄悄自动提交；commit 失败并解绑，之后的普通写照常自动提交。事务里那条与失败那条都不可见。
// commit 失败时库已改发 abortTransaction：服务端当场中止、放锁，收尾的 drop 才不会被那个事务挡住。
// 用单独的集合：万一锁没放，也不拖累后面用 srey_test 的用例(C 与 Lua 两套共用它)。需要副本集(同 _txn_flow)
static int32_t _txn_stale_flow(task_ctx *task, mongo_ctx *mongo) {
    int32_t rtn = ERR_FAILED;
    uint64_t oldskid;
    mongo_session *sess = mongo_startsession(mongo);
    if (NULL == sess) {
        LOG_ERROR("mongo startsession(stale) error.");
        return ERR_FAILED;
    }
    mongo_collection(mongo, "srey_test_stale");
    mongo_drop(mongo, NULL, 0);// 集合本就可能不存在，返回值不判
    if (ERR_OK != mongo_begin(sess)
        || 1 != _insert_id(mongo, 300, "txn-before-drop")) {
        LOG_ERROR("mongo stale: begin / first insert in txn error.");
        goto done;
    }
    oldskid = mongo->sk.skid;
    ev_close(&task->loader->netev, &mongo->sk);
    if (ERR_OK != mongo_ping(mongo)
        || oldskid == mongo->sk.skid) {
        LOG_ERROR("mongo stale: ping did not reconnect.");
        goto done;
    }
    if (_insert_id(mongo, 301, "txn-after-drop") >= 0) {
        LOG_ERROR("mongo stale: write in an invalidated transaction must fail.");
        goto done;
    }
    if (mongo->session != sess) {
        LOG_ERROR("mongo stale: failed write must keep the binding until commit / rollback.");
        goto done;
    }
    if (ERR_OK == mongo_commit(sess, NULL, 0)
        || NULL != mongo->session) {
        LOG_ERROR("mongo stale: commit must fail and unbind.");
        goto done;
    }
    if (1 != _insert_id(mongo, 302, "after-stale")) {
        LOG_ERROR("mongo stale: plain insert after unbind error.");
        goto done;
    }
    if (0 != _count_id(mongo, 300)
        || 0 != _count_id(mongo, 301)
        || 1 != _count_id(mongo, 302)) {
        LOG_ERROR("mongo stale: visibility mismatch (300/301 must be absent, 302 present).");
        goto done;
    }
    // drop 要拿集合排他锁：作废的事务没在服务端中止的话，会被它挡到读超时
    if (ERR_OK != mongo_drop(mongo, NULL, 0)) {
        LOG_ERROR("mongo stale: drop after the aborted transaction error (locks still held?).");
        goto done;
    }
    rtn = ERR_OK;
done:
    mongo_freesession(sess);
    return rtn;
}

static void _recommit_fork(void *owner, void *arg) {
    (void)owner;
    rebegin_arg *ra = (rebegin_arg *)arg;
    ra->commit = mongo_commit(ra->sess, NULL, 0);
}
static void _rebegin_fork(void *owner, void *arg) {
    (void)owner;
    rebegin_arg *ra = (rebegin_arg *)arg;
    ra->rtn = mongo_begin(ra->sess);
}
// commit 挂起等回包期间另一个协程对同一 session 重新 begin：收尾醒来不能清掉新事务的绑定与 options，
// 新事务照常写入、提交。两个子协程按入队顺序起，commit 发出去挂起时 begin 正好跑；
// fork_wait 等两个都跑完才返回，ra 与 sess 不会先于子协程失效。
// 用单独的集合，同 _txn_stale_flow。需要副本集(同 _txn_flow)
static int32_t _txn_rebegin_flow(task_ctx *task, mongo_ctx *mongo) {
    int32_t rtn = ERR_FAILED;
    mongo_session *sess = mongo_startsession(mongo);
    if (NULL == sess) {
        LOG_ERROR("mongo startsession(rebegin) error.");
        return ERR_FAILED;
    }
    mongo_collection(mongo, "srey_test_rebegin");
    mongo_drop(mongo, NULL, 0);// 集合本就可能不存在，返回值不判
    rebegin_arg ra = { sess, ERR_FAILED, ERR_FAILED };
    coro_fn fns[2] = { _recommit_fork, _rebegin_fork };
    void *args[2] = { &ra, &ra };
    if (ERR_OK != mongo_begin(sess)
        || 1 != _insert_id(mongo, 400, "txn-first")) {
        LOG_ERROR("mongo rebegin: begin / first insert in txn error.");
        goto done;
    }
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 2)
        || ERR_OK != ra.commit) {
        LOG_ERROR("mongo rebegin: first commit error.");
        goto done;
    }
    if (ERR_OK != ra.rtn) {
        LOG_ERROR("mongo rebegin: begin during the in-flight commit was rejected.");
        goto done;
    }
    if (mongo->session != sess
        || NULL == sess->options) {
        LOG_ERROR("mongo rebegin: commit wake-up cleared the new transaction.");
        goto done;
    }
    if (1 != _insert_id(mongo, 401, "txn-second")
        || ERR_OK != mongo_commit(sess, NULL, 0)
        || NULL != mongo->session) {
        LOG_ERROR("mongo rebegin: second transaction insert / commit error.");
        goto done;
    }
    if (1 != _count_id(mongo, 400)
        || 1 != _count_id(mongo, 401)) {
        LOG_ERROR("mongo rebegin: visibility mismatch (400/401 must both be present).");
        goto done;
    }
    if (ERR_OK != mongo_drop(mongo, NULL, 0)) {
        LOG_ERROR("mongo rebegin: drop error.");
        goto done;
    }
    rtn = ERR_OK;
done:
    mongo_freesession(sess);
    return rtn;
}

// 先起：占住连接的锁睡一会儿，让后起的那个组好事务写、排上队；再断连重连换掉 skid 后放锁
static void _moved_hold_fork(void *owner, void *arg) {
    (void)owner;
    moved_arg *ma = (moved_arg *)arg;
    coro_serial_ctx *held = ma->mongo->serial;
    if (ERR_OK != coro_serial_enter(held)) {
        return;
    }
    coro_sleep(ma->task, 50);
    ev_close(&ma->task->loader->netev, &ma->mongo->sk);
    ma->reconn = mongo_ping(ma->mongo);// 已持锁，ping 里再上锁按 ref 嵌套
    coro_serial_leave(held);
}
static void _moved_insert_fork(void *owner, void *arg) {
    (void)owner;
    moved_arg *ma = (moved_arg *)arg;
    ma->insert = _insert_id(ma->mongo, 601, "txn-moved");
}
// 组包之后、拿到锁之前连接换过：包里带的是旧事务的上下文，发到新连接会被收进那个已作废的事务，
// 必须在拿到锁后判出来、不发、按失败返回(组包那一刻事务还没作废，组包侧的判定拦不住)。
// 判据是排队那条写返回失败——漏判时它会被收进旧事务、回 n=1。
// rollback 在新连接上发 abort 了结旧事务，两条都不可见。用单独的集合，需要副本集(同 _txn_flow)
static int32_t _txn_moved_flow(task_ctx *task, mongo_ctx *mongo) {
    int32_t rtn = ERR_FAILED;
    uint64_t oldskid;
    mongo_session *sess = mongo_startsession(mongo);
    if (NULL == sess) {
        LOG_ERROR("mongo startsession(moved) error.");
        return ERR_FAILED;
    }
    mongo_collection(mongo, "srey_test_moved");
    mongo_drop(mongo, NULL, 0);// 集合本就可能不存在，返回值不判
    moved_arg ma = { task, mongo, ERR_FAILED, ERR_FAILED };
    coro_fn fns[2] = { _moved_hold_fork, _moved_insert_fork };
    void *args[2] = { &ma, &ma };
    if (ERR_OK != mongo_begin(sess)
        || 1 != _insert_id(mongo, 600, "txn-first")) {
        LOG_ERROR("mongo moved: begin / first insert in txn error.");
        goto done;
    }
    oldskid = mongo->sk.skid;
    if (ERR_OK != coro_fork_wait(coro_task_co(task), fns, args, 2)
        || ERR_OK != ma.reconn
        || oldskid == mongo->sk.skid) {
        LOG_ERROR("mongo moved: holder did not reconnect.");
        goto done;
    }
    if (ma.insert >= 0) {
        LOG_ERROR("mongo moved: write packed before the reconnect must not be sent, got %d.", ma.insert);
        goto done;
    }
    if (mongo->session != sess) {
        LOG_ERROR("mongo moved: dropped write must keep the binding until rollback.");
        goto done;
    }
    // 结果不判：要的是收尾解绑；服务端还留着旧事务时这一下就是把它中止、放锁
    mongo_rollback(sess, NULL, 0);
    if (NULL != mongo->session) {
        LOG_ERROR("mongo moved: rollback on the new connection did not unbind.");
        goto done;
    }
    if (0 != _count_id(mongo, 600)
        || 0 != _count_id(mongo, 601)) {
        LOG_ERROR("mongo moved: visibility mismatch (600/601 must be absent).");
        goto done;
    }
    rtn = ERR_OK;
done:
    mongo_freesession(sess);
    return rtn;
}

// ping 自动重连（含 re-auth）：强制关闭连接后 mongo_ping 应重连并恢复可用，count 验证
static int32_t _reconnect_flow(task_ctx *task, mongo_ctx *mongo) {
    uint64_t oldskid = mongo->sk.skid;
    ev_close(&task->loader->netev, &mongo->sk);
    if (ERR_OK != mongo_ping(mongo)) {
        LOG_ERROR("mongo ping reconnect error.");
        return ERR_FAILED;
    }
    // skid 必须换过：ping 若是"发现连接还在就直接返回成功"，下面的 count 照样过，
    // 而"真的重连了"这条本用例要盯的性质就没人验
    if (oldskid == mongo->sk.skid) {
        LOG_ERROR("mongo ping reconnect: skid unchanged, no reconnect happened.");
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
// 验证读命令组包不带它、照常等回包，且 count 反映 fire-forget 写已生效（同连接顺序处理）
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
    if (ERR_OK != mongo_init(&arg->mongo, arg->host, arg->port, NULL, arg->db)) {
        LOG_ERROR("mongo_init error.");
        return;
    }
    // 凭据必须在 connect 之前设好：hello 与认证都由 mongo_connect 内部完成
    mongo_user_pwd(&arg->mongo, arg->user, arg->password);
    mongo_authdb(&arg->mongo, arg->authdb);
    mongo_authmod(&arg->mongo, "SCRAM-SHA-256");
    ATOMIC_SET((atomic_t *)arg->ok, -2);// 正在连：汇总 task 不把它当终态，连不上再回 0
    if (ERR_OK != mongo_connect(task, &arg->mongo)) {
        LOG_ERROR("mongo connect error.");
        ATOMIC_SET((atomic_t *)arg->ok, 0);
        return;
    }
    // 连上了：此后任何失败都是真失败，不能再被 optional 白名单吞成 network error
    ATOMIC_SET((atomic_t *)arg->ok, -1);
    if (task_isclosing(task)
        || ERR_OK != mongo_ping(&arg->mongo)) {
        LOG_ERROR("mongo ping error.");
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _crud_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _cursor_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _duplicate_key_error(&arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _reconnect_flow(task, &arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _moretocome_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _txn_pack_fail_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _txn_second_session_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _txn_unbound_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _txn_empty_flow(task, &arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    // 事务路径要求 mongo 以副本集运行：docker-compose 的 MONGO_REPLSET 默认 rs0 即满足。
    // 排在全部计数断言之后，故它多插的一行不影响 _crud_flow / _reconnect_flow / _moretocome_flow
    if (task_isclosing(task)
        || ERR_OK != _txn_flow(&arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _txn_reconnect_flow(task, &arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _txn_stale_flow(task, &arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _txn_rebegin_flow(task, &arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    if (task_isclosing(task)
        || ERR_OK != _txn_moved_flow(task, &arg->mongo)) {
        ev_close(&task->loader->netev, &arg->mongo.sk);
        return;
    }
    mongo_quit(&arg->mongo);
    ATOMIC_SET((atomic_t *)arg->ok, 1);
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
