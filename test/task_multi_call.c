#include "task_multi_call.h"

#define N_SUBS 5
#define MSG_BROADCAST "MULTI_HELLO"
#define MSG_LEN 11
#define REQ_TYPE_CALL 200  // task_multi_call 路径
#define REQ_TYPE_RPC  201  // task_multi_request 路径,需要 task_response 回 src
#define RPC_SESS 1u        // multi_request 共用的 sess(snowflake ID 永远 > 1,无碰撞风险)
#define ACK_STR "ack"
#define ACK_LEN 3

typedef struct task_multi_call_args {
    int32_t *ok;
    const char *base_name;
}task_multi_call_args;

// 测试共享计数：所有 subscriber 收到广播后累计
static atomic_t _received_call;     // multi_call 路径
static atomic_t _received_rpc;      // multi_request 路径
static atomic_t _responded_count;   // publisher 收到的响应数

// subscriber 的 request 回调：REQ_TYPE_CALL 仅计数；REQ_TYPE_RPC 计数后 task_response 回 src
static void _sub_requested(task_ctx *task, subtype_t reqtype, uint64_t sess, name_t src,
                           void *data, size_t size) {
    if (MSG_LEN != size || 0 != memcmp(data, MSG_BROADCAST, MSG_LEN)) {
        return;
    }
    if (REQ_TYPE_CALL == reqtype) {
        ATOMIC_ADD(&_received_call, 1);
        return;
    }
    if (REQ_TYPE_RPC == reqtype && INVALID_TNAME != src && 0 != sess) {
        ATOMIC_ADD(&_received_rpc, 1);
        task_ctx *pub = task_grab(task->loader, src);
        if (NULL != pub) {
            task_response(pub, reqtype, sess, ERR_OK, ACK_STR, ACK_LEN, 1);
            task_ungrab(pub);
        }
    }
}

static void _sub_startup(task_ctx *task) {
    task_requested(task, _sub_requested);
}

// publisher 的 _response 回调：在 multi_request 后被调用 N 次（同 sess,各 dst 各自响应）
static void _pub_response(task_ctx *task, subtype_t reqtype, uint64_t sess, int32_t erro, void *data, size_t size) {
    (void)task; (void)erro; (void)reqtype;
    if (RPC_SESS == sess && ACK_LEN == size && 0 == memcmp(data, ACK_STR, ACK_LEN)) {
        ATOMIC_ADD(&_responded_count, 1);
    }
}

static void _pub_startup(task_ctx *task) {
    task_multi_call_args *arg = (task_multi_call_args *)coro_get_arg(task);
    ATOMIC_SET(&_received_call, 0);
    ATOMIC_SET(&_received_rpc, 0);
    ATOMIC_SET(&_responded_count, 0);
    task_responsed(task, _pub_response);
    // 给 subscriber 一点时间完成 startup,确保 _request 回调已挂上
    coro_sleep(task, 100);
    // task_grab 所有 subscriber,组装 dsts 数组;末尾留 NULL 占位验证跳过逻辑
    task_ctx *dsts[N_SUBS + 1];
    int32_t i;
    char subname[64];
    for (i = 0; i < N_SUBS; i++) {
        SNPRINTF(subname, sizeof(subname), "%s_sub%d", arg->base_name, i + 1);
        dsts[i] = task_grab(task->loader, task_find_name(task->loader, subname));
        if (NULL == dsts[i]) {
            LOG_ERROR("multi_call: grab sub %d failed.", i);
            int32_t j;
            for (j = 0; j < i; j++) {
                task_ungrab(dsts[j]);
            }
            return;
        }
    }
    dsts[N_SUBS] = NULL;
    int32_t poll;
    // ── 第 1 段：task_multi_call ────────────────────────────────────────
    task_multi_call(dsts, N_SUBS + 1, REQ_TYPE_CALL, MSG_BROADCAST, MSG_LEN, 1);
    int32_t recvcnt = 0;
    for (poll = 0; poll < 40; poll++) {
        coro_sleep(task, 50);
        recvcnt = (int32_t)ATOMIC_GET(&_received_call);
        if (N_SUBS == recvcnt) {
            break;
        }
    }
    if (N_SUBS != recvcnt) {
        LOG_ERROR("multi_call: received %d/%d.", recvcnt, N_SUBS);
        goto ungrab;
    }
    // ── 第 2 段：task_multi_request,共用 RPC_SESS,subscriber 各自 task_response 回 src ──
    task_multi_request(dsts, N_SUBS + 1, task, REQ_TYPE_RPC, RPC_SESS,
                       MSG_BROADCAST, MSG_LEN, 1);
    int32_t resp = 0;
    int32_t rpc_recv = 0;
    for (poll = 0; poll < 40; poll++) {
        coro_sleep(task, 50);
        resp = (int32_t)ATOMIC_GET(&_responded_count);
        rpc_recv = (int32_t)ATOMIC_GET(&_received_rpc);
        if (N_SUBS == resp && N_SUBS == rpc_recv) {
            break;
        }
    }
    if (N_SUBS != rpc_recv) {
        LOG_ERROR("multi_request: subs received %d/%d.", rpc_recv, N_SUBS);
        goto ungrab;
    }
    if (N_SUBS != resp) {
        LOG_ERROR("multi_request: pub got %d/%d responses.", resp, N_SUBS);
        goto ungrab;
    }
    *(arg->ok) = 1;
    LOG_INFO("multi_call/multi_request tested: call %d/%d, rpc recv %d/%d, resp %d/%d.",
             recvcnt, N_SUBS, rpc_recv, N_SUBS, resp, N_SUBS);
ungrab:
    for (i = 0; i < N_SUBS; i++) {
        task_ungrab(dsts[i]);
    }
}

void task_multi_call_start(loader_ctx *loader, const char *base_name, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    // 先注册 N 个 subscriber：base_name_sub1 .. base_name_subN
    int32_t i;
    char subname[64];
    for (i = 0; i < N_SUBS; i++) {
        SNPRINTF(subname, sizeof(subname), "%s_sub%d", base_name, i + 1);
        coro_task_register(loader, subname, 0, _sub_startup, NULL, NULL, NULL);
    }
    // publisher 用 base_name,放最后注册保证 subscribers 已在 maptasks
    task_multi_call_args *arg;
    CALLOC(arg, 1, sizeof(task_multi_call_args));
    arg->ok = ok;
    arg->base_name = base_name;
    coro_task_register(loader, base_name, 0, _pub_startup, NULL, _free, arg);
}
