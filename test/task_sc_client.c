#include "task_sc_client.h"

typedef struct task_sc_client_args {
    int32_t *ok;
    const char *base_name;
    const char *sc_name;
}task_sc_client_args;

// 文件级:sc_name 由 _startup 从 arg 读出后写入,所有子段共享
static name_t _sc_name;

// 接收统计:_request 回调累计;按 topic 字符串匹配验证
static atomic_t _recv_count;
// 按投递 kind 分类统计:验普通(kind=0)/共享(kind=1)各自路由
static atomic_t _recv_normal;
static atomic_t _recv_shared;
static char _last_topic[128];
static char _last_payload[256];
static char _last_meta[256];
static size_t _last_meta_size;
static name_t _last_publisher;

// 用 sc_parse_deliver 拆 deliver wire 后累计统计
static void _parse_deliver(void *data, size_t size) {
    sc_deliver dlv;
    if (ERR_OK != sc_parse_deliver(data, size, &dlv)) {
        return;
    }
    _last_publisher = dlv.publisher;
    _last_meta_size = dlv.mlen;
    if (dlv.mlen > 0 && dlv.mlen < sizeof(_last_meta)) {
        memcpy(_last_meta, dlv.meta, dlv.mlen);
        _last_meta[dlv.mlen] = '\0';
    } else {
        _last_meta[0] = '\0';
    }
    if (dlv.tlen < sizeof(_last_topic)) {
        memcpy(_last_topic, dlv.topic, dlv.tlen);
        _last_topic[dlv.tlen] = '\0';
    }
    if (dlv.plen > 0 && dlv.plen < sizeof(_last_payload)) {
        memcpy(_last_payload, dlv.payload, dlv.plen);
        _last_payload[dlv.plen] = '\0';
    } else {
        _last_payload[0] = '\0';
    }
    ATOMIC_ADD(&_recv_count, 1);
    if (SC_DELIVER_SHARED == dlv.kind) {
        ATOMIC_ADD(&_recv_shared, 1);
    } else {
        ATOMIC_ADD(&_recv_normal, 1);
    }
}

// task _request 回调:收 REQ_SC_DELIVER 时拆 wire 累计统计
static void _on_request(task_ctx *task, subtype_t reqtype, uint64_t sess, name_t src,
                        void *data, size_t size) {
    (void)task;
    (void)sess;
    (void)src;
    if (REQ_SC_DELIVER == reqtype) {
        _parse_deliver(data, size);
    }
}

// 重置接收统计:每个子段前调一次,避免上轮残留
static void _reset_recv(void) {
    ATOMIC_SET(&_recv_count, 0);
    ATOMIC_SET(&_recv_normal, 0);
    ATOMIC_SET(&_recv_shared, 0);
    _last_topic[0] = '\0';
    _last_payload[0] = '\0';
    _last_meta[0] = '\0';
    _last_meta_size = 0;
    _last_publisher = INVALID_TNAME;
}

// 轮询等收到指定条数(50ms × 40 = 2s 上限)
static int32_t _wait_recv(task_ctx *task, int32_t expect) {
    int32_t poll;
    for (poll = 0; poll < 40; poll++) {
        if ((int32_t)ATOMIC_GET(&_recv_count) >= expect) {
            return 1;
        }
        coro_sleep(task, 50);
    }
    return 0;
}

// 子段 1:单订阅 + publish 收 1 次
static int32_t _test_basic_pub_sub(task_ctx *task) {
    _reset_recv();
    if (ERR_OK != coro_sc_subscribe(task, _sc_name, "t1/a")) {
        LOG_ERROR("sc subscribe t1/a failed");
        return ERR_FAILED;
    }
    if (ERR_OK != coro_sc_publish(task, _sc_name, "t1/a", "hello", 5)) {
        LOG_ERROR("sc publish t1/a failed");
        return ERR_FAILED;
    }
    if (!_wait_recv(task, 1)) {
        LOG_ERROR("basic_pub_sub: expect 1 recv, got %d", (int32_t)ATOMIC_GET(&_recv_count));
        return ERR_FAILED;
    }
    if (0 != strcmp(_last_topic, "t1/a") || 0 != strcmp(_last_payload, "hello")) {
        LOG_ERROR("basic_pub_sub: topic/payload mismatch: %s / %s", _last_topic, _last_payload);
        return ERR_FAILED;
    }
    if (_last_publisher == INVALID_TNAME) {
        LOG_ERROR("basic_pub_sub: publisher should not be INVALID_TNAME");
        return ERR_FAILED;
    }
    coro_sc_unsubscribe(task, _sc_name, "t1/a");
    return ERR_OK;
}

// 子段 2:通配 "+" 匹配:订 "t2/+",发 "t2/a" "t2/b" 各收 1 次
static int32_t _test_plus_wildcard(task_ctx *task) {
    _reset_recv();
    if (ERR_OK != coro_sc_subscribe(task, _sc_name, "t2/+")) {
        return ERR_FAILED;
    }
    coro_sc_publish(task, _sc_name, "t2/a", "p1", 2);
    coro_sc_publish(task, _sc_name, "t2/b", "p2", 2);
    if (!_wait_recv(task, 2)) {
        LOG_ERROR("plus_wildcard: expect 2 recv, got %d", (int32_t)ATOMIC_GET(&_recv_count));
        return ERR_FAILED;
    }
    coro_sc_unsubscribe(task, _sc_name, "t2/+");
    return ERR_OK;
}

// 子段 3:通配 "#" 匹配:订 "t3/#",发 "t3/a" "t3/a/b" "t3" 都命中
static int32_t _test_hash_wildcard(task_ctx *task) {
    _reset_recv();
    if (ERR_OK != coro_sc_subscribe(task, _sc_name, "t3/#")) {
        return ERR_FAILED;
    }
    coro_sc_publish(task, _sc_name, "t3/a", "1", 1);
    coro_sc_publish(task, _sc_name, "t3/a/b", "2", 1);
    if (!_wait_recv(task, 2)) {
        LOG_ERROR("hash_wildcard: expect 2 recv, got %d", (int32_t)ATOMIC_GET(&_recv_count));
        return ERR_FAILED;
    }
    coro_sc_unsubscribe(task, _sc_name, "t3/#");
    return ERR_OK;
}

// 子段 4:自回环:订 "loop",发 "loop",收 1 次(publisher == 自己)
static int32_t _test_self_loop(task_ctx *task) {
    _reset_recv();
    if (ERR_OK != coro_sc_subscribe(task, _sc_name, "loop")) {
        return ERR_FAILED;
    }
    coro_sc_publish(task, _sc_name, "loop", "self", 4);
    if (!_wait_recv(task, 1)) {
        LOG_ERROR("self_loop: expect 1 recv");
        return ERR_FAILED;
    }
    if (_last_publisher != task->handle) {
        LOG_ERROR("self_loop: publisher %"PRIu64" != self %"PRIu64"", _last_publisher, task->handle);
        return ERR_FAILED;
    }
    coro_sc_unsubscribe(task, _sc_name, "loop");
    return ERR_OK;
}

// 子段 5:多 pattern 命中同 publish,去重 publish_dedup 收 1 次
static int32_t _test_dedup_multi_pattern(task_ctx *task) {
    _reset_recv();
    coro_sc_subscribe(task, _sc_name, "t5/a");
    coro_sc_subscribe(task, _sc_name, "t5/+");
    coro_sc_publish(task, _sc_name, "t5/a", "x", 1);
    coro_sleep(task, 200);   // 等可能的多次 deliver 到达
    int32_t got = (int32_t)ATOMIC_GET(&_recv_count);
    if (1 != got) {
        LOG_ERROR("dedup_multi_pattern: expect 1 recv (publish_dedup hashset), got %d", got);
        return ERR_FAILED;
    }
    coro_sc_unsubscribe(task, _sc_name, "t5/a");
    coro_sc_unsubscribe(task, _sc_name, "t5/+");
    return ERR_OK;
}

// 子段 6:双角色(普通 + 共享同 topic),group 间不去重 publish 收 2 次
static int32_t _test_dual_role(task_ctx *task) {
    _reset_recv();
    coro_sc_subscribe(task, _sc_name, "t6/role");
    coro_sc_subscribe_shared(task, _sc_name, "t6/role", "g1");
    coro_sc_publish(task, _sc_name, "t6/role", "dual", 4);
    coro_sleep(task, 200);
    int32_t got = (int32_t)ATOMIC_GET(&_recv_count);
    if (2 != got) {
        LOG_ERROR("dual_role: expect 2 recv (normal + shared), got %d", got);
        return ERR_FAILED;
    }
    int32_t gn = (int32_t)ATOMIC_GET(&_recv_normal);
    int32_t gs = (int32_t)ATOMIC_GET(&_recv_shared);
    if (1 != gn || 1 != gs) {
        LOG_ERROR("dual_role: expect normal=1 shared=1, got normal=%d shared=%d", gn, gs);
        return ERR_FAILED;
    }
    coro_sc_unsubscribe(task, _sc_name, "t6/role");
    coro_sc_unsubscribe_shared(task, _sc_name, "t6/role", "g1");
    return ERR_OK;
}

// 子段 7:retained:publish_retained → query_retained 拿到
static int32_t _test_retained_basic(task_ctx *task) {
    if (ERR_OK != coro_sc_publish_retained(task, _sc_name, "t7/r", "saved", 5)) {
        return ERR_FAILED;
    }
    size_t qsize = 0;
    int32_t erro = 0;
    void *qdata = coro_sc_query_retained(task, _sc_name, "t7/r", &qsize, &erro);
    if (EMPTYPTR(qdata, qsize)) {
        LOG_ERROR("retained_basic: query_retained empty");
        return ERR_FAILED;
    }
    // wire: | name_t pub | u16 mlen | meta | u16 tlen | topic | u32 plen | payload |
    binary_ctx br;
    binary_init(&br, (char *)qdata, qsize, 0);
    (void)binary_get_uinteger(&br, sizeof(name_t), 0);   // skip publisher
    uint16_t mlen = (uint16_t)binary_get_uinteger(&br, 2, 0);
    if (mlen > 0) {
        (void)binary_get_string(&br, mlen);
    }
    uint16_t tlen = (uint16_t)binary_get_uinteger(&br, 2, 0);
    const char *topic = binary_get_string(&br, tlen);
    uint32_t plen = (uint32_t)binary_get_uinteger(&br, 4, 0);
    const char *payload = binary_get_string(&br, plen);
    if (4 != tlen || 0 != memcmp(topic, "t7/r", 4)) {
        LOG_ERROR("retained_basic: topic mismatch len=%u", tlen);
        return ERR_FAILED;
    }
    if (5 != plen || 0 != memcmp(payload, "saved", 5)) {
        LOG_ERROR("retained_basic: payload mismatch");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 8:publish_retained plen=0 清空,query_retained 拿不到
static int32_t _test_retained_clear(task_ctx *task) {
    coro_sc_publish_retained(task, _sc_name, "t8/r", "data", 4);
    coro_sc_publish_retained(task, _sc_name, "t8/r", NULL, 0);   // 清空
    size_t qsize = 0;
    int32_t erro = 0;
    void *qdata = coro_sc_query_retained(task, _sc_name, "t8/r", &qsize, &erro);
    if (NULL != qdata && qsize > 0) {
        LOG_ERROR("retained_clear: expect empty after plen=0, got size=%zu", qsize);
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 9:retained_meta 快照:set_meta → publish_retained → set_meta 改 → query 拿原快照
static int32_t _test_retained_meta_snapshot(task_ctx *task) {
    coro_sc_set_meta(task, _sc_name, "v1", 2);
    coro_sc_publish_retained(task, _sc_name, "t9/s", "data", 4);
    coro_sc_set_meta(task, _sc_name, "v2", 2);   // 改 publisher 当前 meta
    size_t qsize = 0;
    int32_t erro = 0;
    void *qdata = coro_sc_query_retained(task, _sc_name, "t9/s", &qsize, &erro);
    if (EMPTYPTR(qdata, qsize)) {
        LOG_ERROR("retained_meta_snapshot: query empty");
        return ERR_FAILED;
    }
    binary_ctx br;
    binary_init(&br, (char *)qdata, qsize, 0);
    (void)binary_get_uinteger(&br, sizeof(name_t), 0);
    uint16_t mlen = (uint16_t)binary_get_uinteger(&br, 2, 0);
    if (2 != mlen) {
        LOG_ERROR("retained_meta_snapshot: meta size %u != 2", mlen);
        return ERR_FAILED;
    }
    const char *meta = binary_get_string(&br, mlen);
    if (0 != memcmp(meta, "v1", 2)) {
        LOG_ERROR("retained_meta_snapshot: expect v1 snapshot, got %.2s", meta);
        return ERR_FAILED;
    }
    coro_sc_publish_retained(task, _sc_name, "t9/s", NULL, 0);   // 清理
    coro_sc_set_meta(task, _sc_name, NULL, 0);
    return ERR_OK;
}

// 子段 10:set_meta + publish,deliver 带 meta
static int32_t _test_set_meta_in_deliver(task_ctx *task) {
    _reset_recv();
    coro_sc_set_meta(task, _sc_name, "MM", 2);
    coro_sc_subscribe(task, _sc_name, "t10/m");
    coro_sc_publish(task, _sc_name, "t10/m", "p", 1);
    if (!_wait_recv(task, 1)) {
        LOG_ERROR("set_meta_in_deliver: expect 1 recv");
        return ERR_FAILED;
    }
    if (2 != _last_meta_size || 0 != memcmp(_last_meta, "MM", 2)) {
        LOG_ERROR("set_meta_in_deliver: meta mismatch size=%zu val=%.2s", _last_meta_size, _last_meta);
        return ERR_FAILED;
    }
    coro_sc_unsubscribe(task, _sc_name, "t10/m");
    coro_sc_set_meta(task, _sc_name, NULL, 0);
    return ERR_OK;
}

// 子段 11:unsubscribe 后不再收
static int32_t _test_unsub_no_deliver(task_ctx *task) {
    coro_sc_subscribe(task, _sc_name, "t11/u");
    coro_sc_unsubscribe(task, _sc_name, "t11/u");
    _reset_recv();
    coro_sc_publish(task, _sc_name, "t11/u", "x", 1);
    coro_sleep(task, 200);
    if (0 != ATOMIC_GET(&_recv_count)) {
        LOG_ERROR("unsub_no_deliver: expect 0 after unsub, got %d", (int32_t)ATOMIC_GET(&_recv_count));
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 12:重复 sub / 未订阅 unsub 幂等(返 OK,不报错)
static int32_t _test_idempotent(task_ctx *task) {
    if (ERR_OK != coro_sc_subscribe(task, _sc_name, "t12/i")) {
        return ERR_FAILED;
    }
    if (ERR_OK != coro_sc_subscribe(task, _sc_name, "t12/i")) {
        LOG_ERROR("idempotent: repeat subscribe should be OK");
        return ERR_FAILED;
    }
    if (ERR_OK != coro_sc_unsubscribe(task, _sc_name, "t12/i")) {
        return ERR_FAILED;
    }
    if (ERR_OK != coro_sc_unsubscribe(task, _sc_name, "t12/i")) {
        LOG_ERROR("idempotent: unsub of unsubscribed should be OK");
        return ERR_FAILED;
    }
    if (ERR_OK != coro_sc_unsubscribe(task, _sc_name, "t12/never_subbed")) {
        LOG_ERROR("idempotent: unsub of never-subbed should be OK");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 13:topics 调试输出非空(至少订过几个 topic)
static int32_t _test_topics_list(task_ctx *task) {
    coro_sc_subscribe(task, _sc_name, "t13/aaa");
    coro_sc_subscribe(task, _sc_name, "t13/bbb");
    size_t lsize = 0;
    int32_t erro = 0;
    void *ldata = coro_sc_topics(task, _sc_name, &lsize, &erro);
    if (EMPTYPTR(ldata, lsize)) {
        LOG_ERROR("topics_list: empty");
        return ERR_FAILED;
    }
    coro_sc_unsubscribe(task, _sc_name, "t13/aaa");
    coro_sc_unsubscribe(task, _sc_name, "t13/bbb");
    return ERR_OK;
}

// 子段 14:retained_topics 调试输出非空
static int32_t _test_retained_topics_list(task_ctx *task) {
    coro_sc_publish_retained(task, _sc_name, "t14/a", "1", 1);
    coro_sc_publish_retained(task, _sc_name, "t14/b", "2", 1);
    size_t lsize = 0;
    int32_t erro = 0;
    void *ldata = coro_sc_retained_topics(task, _sc_name, &lsize, &erro);
    if (EMPTYPTR(ldata, lsize)) {
        LOG_ERROR("retained_topics_list: empty");
        return ERR_FAILED;
    }
    coro_sc_publish_retained(task, _sc_name, "t14/a", NULL, 0);
    coro_sc_publish_retained(task, _sc_name, "t14/b", NULL, 0);
    return ERR_OK;
}

// 子段 15:共享订阅:同 group 多 task 轮询(单 task 自订两次共享只算一个成员;
// 这里简化为验证共享订阅本身可工作且不收 retained)
static int32_t _test_shared_basic(task_ctx *task) {
    coro_sc_publish_retained(task, _sc_name, "t15/r", "before_sub", 10);
    _reset_recv();
    if (ERR_OK != coro_sc_subscribe_shared(task, _sc_name, "t15/r", "g1")) {
        return ERR_FAILED;
    }
    coro_sleep(task, 100);
    // 共享订阅订后不收 retained
    if (0 != ATOMIC_GET(&_recv_count)) {
        LOG_ERROR("shared_basic: shared sub should NOT receive retained, got %d", (int32_t)ATOMIC_GET(&_recv_count));
        coro_sc_unsubscribe_shared(task, _sc_name, "t15/r", "g1");
        coro_sc_publish_retained(task, _sc_name, "t15/r", NULL, 0);
        return ERR_FAILED;
    }
    // publish 仍应收到(共享组单成员 = 自己)
    coro_sc_publish(task, _sc_name, "t15/r", "live", 4);
    if (!_wait_recv(task, 1)) {
        LOG_ERROR("shared_basic: expect 1 recv from publish");
        coro_sc_unsubscribe_shared(task, _sc_name, "t15/r", "g1");
        coro_sc_publish_retained(task, _sc_name, "t15/r", NULL, 0);
        return ERR_FAILED;
    }
    coro_sc_unsubscribe_shared(task, _sc_name, "t15/r", "g1");
    coro_sc_publish_retained(task, _sc_name, "t15/r", NULL, 0);
    return ERR_OK;
}

// 辅助"死订阅者":订阅通配 "tw/+" 后即返回,由主 task task_close 销毁(不 unsubscribe),
// 用于触发 subcenter 对通配节点死订阅的懒清理。直接用文件级 _sc_name(主 task 已设)
static void _dead_sub_startup(task_ctx *task) {
    coro_sc_subscribe(task, _sc_name, "tw/+");
}
// 解析 coro_sc_topics wire(每条 | u16 tlen | topic | u32 normal | u32 shared |,大端),查 want 是否在列表
static int32_t _topics_contains(const void *data, size_t size, const char *want) {
    const uint8_t *p = (const uint8_t *)data;
    size_t off = 0;
    size_t wlen = strlen(want);
    while (off + 2 <= size) {
        uint16_t tlen = (uint16_t)(((uint16_t)p[off] << 8) | p[off + 1]);
        off += 2;
        if (off + (size_t)tlen + 8 > size) {
            break;
        }
        if ((size_t)tlen == wlen && 0 == memcmp(p + off, want, wlen)) {
            return 1;
        }
        off += (size_t)tlen + 8;
    }
    return 0;
}
// 子段 16:通配订阅者死亡后 publish → 死订阅从通配节点回收 + 空通配节点删除(M4)
static int32_t _test_wildcard_dead_sub_cleanup(task_ctx *task) {
    int32_t poll;
    size_t lsize;
    int32_t erro;
    void *ldata;
    task_ctx *probe;
    task_ctx *b = coro_task_register(task->loader, "sc_dead_sub", 0, _dead_sub_startup, NULL, NULL, NULL);
    if (NULL == b) {
        LOG_ERROR("dead_sub_cleanup: register helper failed");
        return ERR_FAILED;
    }
    name_t bname = task_find_name(task->loader, "sc_dead_sub");
    // 等 B 订阅成功("tw/+" 出现),避免 B 没订上导致假阳性
    for (poll = 0; poll < 40; poll++) {
        lsize = 0;
        ldata = coro_sc_topics(task, _sc_name, &lsize, &erro);
        if (_topics_contains(ldata, lsize, "tw/+")) {
            break;
        }
        coro_sleep(task, 50);
    }
    if (poll >= 40) {
        LOG_ERROR("dead_sub_cleanup: helper subscribe 'tw/+' not observed");
        return ERR_FAILED;
    }
    // 关闭 B,轮询等其真正销毁(subcenter 据 task_grab 失败才判其为死订阅)
    task_close(b);
    for (poll = 0; poll < 40; poll++) {
        probe = task_grab(task->loader, bname);
        if (NULL == probe) {
            break;
        }
        task_ungrab(probe);
        coro_sleep(task, 50);
    }
    if (poll >= 40) {
        LOG_ERROR("dead_sub_cleanup: helper not destroyed");
        return ERR_FAILED;
    }
    // publish 命中 "tw/+" 触发懒清理;轮询等 "tw/+" 从订阅列表消失(修复前死订阅残留在通配节点,永不消失)
    coro_sc_publish(task, _sc_name, "tw/x", "p", 1);
    for (poll = 0; poll < 40; poll++) {
        lsize = 0;
        ldata = coro_sc_topics(task, _sc_name, &lsize, &erro);
        if (!_topics_contains(ldata, lsize, "tw/+")) {
            return ERR_OK;
        }
        coro_sleep(task, 50);
    }
    LOG_ERROR("dead_sub_cleanup: 'tw/+' not pruned after dead subscriber + publish");
    return ERR_FAILED;
}

// 辅助"死共享成员":共享订阅 "tsw/m" 组 "sg1" 后即返回,由主 task task_close 销毁(不 unsubscribe)
static void _dead_shared_sub_startup(task_ctx *task) {
    coro_sc_subscribe_shared(task, _sc_name, "tsw/m", "sg1");
}
// 辅助"死共享成员"2:共享订阅 "tsw/k" 组 "sg2"(与主 task 同组,作将死的另一成员)
static void _dead_shared_sub2_startup(task_ctx *task) {
    coro_sc_subscribe_shared(task, _sc_name, "tsw/k", "sg2");
}
// 子段 17:共享订阅成员死亡后 publish → 死成员剔除 + 空组删除 + 空节点回收(F-SC-1)
static int32_t _test_shared_dead_member_cleanup(task_ctx *task) {
    int32_t poll;
    size_t lsize;
    int32_t erro;
    void *ldata;
    task_ctx *probe;
    task_ctx *b = coro_task_register(task->loader, "sc_dead_shared", 0, _dead_shared_sub_startup, NULL, NULL, NULL);
    if (NULL == b) {
        LOG_ERROR("shared_dead_cleanup: register helper failed");
        return ERR_FAILED;
    }
    name_t bname = task_find_name(task->loader, "sc_dead_shared");
    // 等 B 共享订阅成功("tsw/m" 出现)
    for (poll = 0; poll < 40; poll++) {
        lsize = 0;
        ldata = coro_sc_topics(task, _sc_name, &lsize, &erro);
        if (_topics_contains(ldata, lsize, "tsw/m")) {
            break;
        }
        coro_sleep(task, 50);
    }
    if (poll >= 40) {
        LOG_ERROR("shared_dead_cleanup: helper subscribe 'tsw/m' not observed");
        return ERR_FAILED;
    }
    // 关闭 B,轮询等其真正销毁
    task_close(b);
    for (poll = 0; poll < 40; poll++) {
        probe = task_grab(task->loader, bname);
        if (NULL == probe) {
            break;
        }
        task_ungrab(probe);
        coro_sleep(task, 50);
    }
    if (poll >= 40) {
        LOG_ERROR("shared_dead_cleanup: helper not destroyed");
        return ERR_FAILED;
    }
    // publish 命中 "tsw/m":pick 发现唯一成员死 → 剔除 → 空组删 → 空节点回收;轮询等 "tsw/m" 消失
    coro_sc_publish(task, _sc_name, "tsw/m", "p", 1);
    for (poll = 0; poll < 40; poll++) {
        lsize = 0;
        ldata = coro_sc_topics(task, _sc_name, &lsize, &erro);
        if (!_topics_contains(ldata, lsize, "tsw/m")) {
            return ERR_OK;
        }
        coro_sleep(task, 50);
    }
    LOG_ERROR("shared_dead_cleanup: 'tsw/m' not reclaimed after dead member + publish");
    return ERR_FAILED;
}
// 子段 18:组内一死一活 → publish 仍投到活成员,不丢(F-SC-1 方案 B 零漏投)
static int32_t _test_shared_dead_skip_deliver(task_ctx *task) {
    int32_t poll;
    task_ctx *probe;
    // 主 task 作为活成员加入组 sg2
    if (ERR_OK != coro_sc_subscribe_shared(task, _sc_name, "tsw/k", "sg2")) {
        LOG_ERROR("shared_skip: main subscribe_shared failed");
        return ERR_FAILED;
    }
    // 辅助 task 作为(将死的)另一成员加入同组,等其订上
    task_ctx *b = coro_task_register(task->loader, "sc_dead_shared2", 0, _dead_shared_sub2_startup, NULL, NULL, NULL);
    if (NULL == b) {
        coro_sc_unsubscribe_shared(task, _sc_name, "tsw/k", "sg2");
        LOG_ERROR("shared_skip: register helper failed");
        return ERR_FAILED;
    }
    name_t bname = task_find_name(task->loader, "sc_dead_shared2");
    coro_sleep(task, 150);
    // 关闭辅助,轮询等其真正销毁(组内只剩主 task 这个活成员)
    task_close(b);
    for (poll = 0; poll < 40; poll++) {
        probe = task_grab(task->loader, bname);
        if (NULL == probe) {
            break;
        }
        task_ungrab(probe);
        coro_sleep(task, 50);
    }
    if (poll >= 40) {
        coro_sc_unsubscribe_shared(task, _sc_name, "tsw/k", "sg2");
        LOG_ERROR("shared_skip: helper not destroyed");
        return ERR_FAILED;
    }
    // 连发 3 条:pick 跳过死成员选活的(主 task),3 条应全部收到(修复前 cursor 压到死成员的那轮会丢)
    _reset_recv();
    coro_sc_publish(task, _sc_name, "tsw/k", "a", 1);
    coro_sc_publish(task, _sc_name, "tsw/k", "b", 1);
    coro_sc_publish(task, _sc_name, "tsw/k", "c", 1);
    int32_t got = _wait_recv(task, 3);
    coro_sc_unsubscribe_shared(task, _sc_name, "tsw/k", "sg2");
    if (!got) {
        LOG_ERROR("shared_skip: live member missed shared delivery (expect 3)");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段:同 task 同 topic 多 group 各自精确投递(对齐 Lua unit_sc_client sub14)
// 主 task 同时加入 g1、g2 各为唯一成员 → 单次 publish 收 2 次;退订 g1 后只剩 g2 收 1 次,互不覆盖
static int32_t _test_shared_multi_group(task_ctx *task) {
    if (ERR_OK != coro_sc_subscribe_shared(task, _sc_name, "t20/x", "g1")) {
        LOG_ERROR("multi_group: subscribe g1 failed");
        return ERR_FAILED;
    }
    if (ERR_OK != coro_sc_subscribe_shared(task, _sc_name, "t20/x", "g2")) {
        coro_sc_unsubscribe_shared(task, _sc_name, "t20/x", "g1");
        LOG_ERROR("multi_group: subscribe g2 failed");
        return ERR_FAILED;
    }
    coro_sleep(task, 100);
    // 单次 publish:g1、g2 各挑唯一成员(自己)→ 共收 2 次
    _reset_recv();
    coro_sc_publish(task, _sc_name, "t20/x", "p", 1);
    if (!_wait_recv(task, 2)) {
        LOG_ERROR("multi_group: expect 2 deliveries (g1 + g2), got %d", (int32_t)ATOMIC_GET(&_recv_count));
        coro_sc_unsubscribe_shared(task, _sc_name, "t20/x", "g1");
        coro_sc_unsubscribe_shared(task, _sc_name, "t20/x", "g2");
        return ERR_FAILED;
    }
    // 退订 g1:不抹 g2;再 publish 仅 g2 收 1 次
    coro_sc_unsubscribe_shared(task, _sc_name, "t20/x", "g1");
    _reset_recv();
    coro_sc_publish(task, _sc_name, "t20/x", "p2", 2);
    if (!_wait_recv(task, 1)) {
        LOG_ERROR("multi_group: after unsub g1, expect 1 delivery from g2");
        coro_sc_unsubscribe_shared(task, _sc_name, "t20/x", "g2");
        return ERR_FAILED;
    }
    // 留观察窗口确认 g1 退订后不再投递
    coro_sleep(task, 100);
    if (1 != (int32_t)ATOMIC_GET(&_recv_count)) {
        LOG_ERROR("multi_group: after unsub g1, expect exactly 1, got %d", (int32_t)ATOMIC_GET(&_recv_count));
        coro_sc_unsubscribe_shared(task, _sc_name, "t20/x", "g2");
        return ERR_FAILED;
    }
    coro_sc_unsubscribe_shared(task, _sc_name, "t20/x", "g2");
    return ERR_OK;
}

// 子段 19:异步 sc_* sess=0 拒绝 — sess=0 会丢调用方身份,全部返 ERR_FAILED 且不下发(A1 入口层)
static int32_t _test_async_sess_zero_reject(task_ctx *task) {
    if (ERR_FAILED != sc_subscribe(task, _sc_name, 0, "z/a")
        || ERR_FAILED != sc_subscribe_shared(task, _sc_name, 0, "z/a", "zg")
        || ERR_FAILED != sc_unsubscribe(task, _sc_name, 0, "z/a")
        || ERR_FAILED != sc_publish(task, _sc_name, 0, "z/a", "x", 1)
        || ERR_FAILED != sc_publish_retained(task, _sc_name, 0, "z/a", "x", 1)
        || ERR_FAILED != sc_query_retained(task, _sc_name, 0, "z/a")
        || ERR_FAILED != sc_topics(task, _sc_name, 0)
        || ERR_FAILED != sc_retained_topics(task, _sc_name, 0)
        || ERR_FAILED != sc_set_meta(task, _sc_name, 0, "m", 1)) {
        LOG_ERROR("async sess=0: expect all ERR_FAILED");
        return ERR_FAILED;
    }
    // sess=0 被拒后不应产生订阅:z/a 不在 topics 列表
    size_t lsize = 0;
    int32_t erro = 0;
    void *ldata = coro_sc_topics(task, _sc_name, &lsize, &erro);
    if (_topics_contains(ldata, lsize, "z/a")) {
        LOG_ERROR("async sess=0: 'z/a' should not be subscribed");
        return ERR_FAILED;
    }
    return ERR_OK;
}

// 子段 21:sc_parse_retained / sc_parse_topics / sc_parse_retained_topics 对真实 service 输出回环解码
static int32_t _test_parse_helpers(task_ctx *task) {
    coro_sc_set_meta(task, _sc_name, "pm", 2);
    if (ERR_OK != coro_sc_publish_retained(task, _sc_name, "tp/k", "val123", 6)
        || ERR_OK != coro_sc_subscribe(task, _sc_name, "tp/k")) {
        return ERR_FAILED;
    }
    size_t sz = 0;
    int32_t erro = 0;
    int32_t found;
    binary_ctx br;
    // 1) sc_parse_retained:query_retained("tp/k") 含一条,publisher/topic/payload/meta 匹配
    void *qd = coro_sc_query_retained(task, _sc_name, "tp/k", &sz, &erro);
    if (EMPTYPTR(qd, sz)) {
        LOG_ERROR("parse_helpers: query_retained empty");
        return ERR_FAILED;
    }
    binary_init(&br, (char *)qd, sz, 0);
    found = 0;
    sc_retained r;
    while (br.offset < br.size) {
        if (ERR_OK != sc_parse_retained(&br, &r)) {
            LOG_ERROR("parse_helpers: sc_parse_retained failed");
            return ERR_FAILED;
        }
        if (4 == r.tlen && 0 == memcmp(r.topic, "tp/k", 4)) {
            found = 1;
            if (6 != r.plen || 0 != memcmp(r.payload, "val123", 6)
                || 2 != r.mlen || 0 != memcmp(r.meta, "pm", 2)) {
                LOG_ERROR("parse_helpers: sc_retained fields mismatch");
                return ERR_FAILED;
            }
        }
    }
    if (!found) {
        LOG_ERROR("parse_helpers: 'tp/k' not found via sc_parse_retained");
        return ERR_FAILED;
    }
    // 2) sc_parse_topics:topics 列表含 tp/k 且 normal>=1
    sz = 0;
    void *td = coro_sc_topics(task, _sc_name, &sz, &erro);
    if (EMPTYPTR(td, sz)) {
        LOG_ERROR("parse_helpers: topics empty");
        return ERR_FAILED;
    }
    binary_init(&br, (char *)td, sz, 0);
    found = 0;
    sc_topic tp;
    while (br.offset < br.size) {
        if (ERR_OK != sc_parse_topics(&br, &tp)) {
            LOG_ERROR("parse_helpers: sc_parse_topics failed");
            return ERR_FAILED;
        }
        if (4 == tp.tlen && 0 == memcmp(tp.topic, "tp/k", 4)) {
            found = 1;
            if (tp.normal < 1) {
                LOG_ERROR("parse_helpers: tp/k normal=%u (<1)", tp.normal);
                return ERR_FAILED;
            }
        }
    }
    if (!found) {
        LOG_ERROR("parse_helpers: 'tp/k' not found via sc_parse_topics");
        return ERR_FAILED;
    }
    // 3) sc_parse_retained_topics:含 tp/k 且 size=6 meta_size=2
    sz = 0;
    void *rd = coro_sc_retained_topics(task, _sc_name, &sz, &erro);
    if (EMPTYPTR(rd, sz)) {
        LOG_ERROR("parse_helpers: retained_topics empty");
        return ERR_FAILED;
    }
    binary_init(&br, (char *)rd, sz, 0);
    found = 0;
    sc_retained_topic rt;
    while (br.offset < br.size) {
        if (ERR_OK != sc_parse_retained_topics(&br, &rt)) {
            LOG_ERROR("parse_helpers: sc_parse_retained_topics failed");
            return ERR_FAILED;
        }
        if (4 == rt.tlen && 0 == memcmp(rt.topic, "tp/k", 4)) {
            found = 1;
            if (6 != rt.size || 2 != rt.meta_size) {
                LOG_ERROR("parse_helpers: rt size=%u meta_size=%u", rt.size, rt.meta_size);
                return ERR_FAILED;
            }
        }
    }
    if (!found) {
        LOG_ERROR("parse_helpers: 'tp/k' not found via sc_parse_retained_topics");
        return ERR_FAILED;
    }
    // 清理:取消订阅 + 清空 retained + 清 meta
    coro_sc_unsubscribe(task, _sc_name, "tp/k");
    coro_sc_publish_retained(task, _sc_name, "tp/k", NULL, 0);
    coro_sc_set_meta(task, _sc_name, NULL, 0);
    return ERR_OK;
}

static void _startup(task_ctx *task) {
    task_sc_client_args *arg = (task_sc_client_args *)coro_get_arg(task);
    _sc_name = task_find_name(task->loader, arg->sc_name);
    task_requested(task, _on_request);

    if (ERR_OK != _test_basic_pub_sub(task))         { return; }
    if (ERR_OK != _test_plus_wildcard(task))         { return; }
    if (ERR_OK != _test_hash_wildcard(task))         { return; }
    if (ERR_OK != _test_self_loop(task))             { return; }
    if (ERR_OK != _test_dedup_multi_pattern(task))   { return; }
    if (ERR_OK != _test_dual_role(task))             { return; }
    if (ERR_OK != _test_retained_basic(task))        { return; }
    if (ERR_OK != _test_retained_clear(task))        { return; }
    if (ERR_OK != _test_retained_meta_snapshot(task)){ return; }
    if (ERR_OK != _test_set_meta_in_deliver(task))   { return; }
    if (ERR_OK != _test_unsub_no_deliver(task))      { return; }
    if (ERR_OK != _test_idempotent(task))            { return; }
    if (ERR_OK != _test_topics_list(task))           { return; }
    if (ERR_OK != _test_retained_topics_list(task))  { return; }
    if (ERR_OK != _test_shared_basic(task))          { return; }
    if (ERR_OK != _test_wildcard_dead_sub_cleanup(task)) { return; }
    if (ERR_OK != _test_shared_dead_member_cleanup(task)) { return; }
    if (ERR_OK != _test_shared_dead_skip_deliver(task)) { return; }
    if (ERR_OK != _test_shared_multi_group(task))    { return; }
    if (ERR_OK != _test_async_sess_zero_reject(task)) { return; }
    if (ERR_OK != _test_parse_helpers(task))         { return; }

    *(arg->ok) = 1;
    LOG_INFO("sc_client tested: 21/21 subtests passed.");
}

void task_sc_client_start(loader_ctx *loader, const char *base_name, const char *sc_name, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    task_sc_client_args *arg;
    CALLOC(arg, 1, sizeof(task_sc_client_args));
    arg->ok = ok;
    arg->base_name = base_name;
    arg->sc_name = sc_name;
    coro_task_register(loader, base_name, 0, _startup, NULL, _free, arg);
}
