#include "srey/debug_request.h"
#include "srey/coro.h"
#include "serial/seri.h"
#include "utils/binary.h"
#include "utils/log.h"

// stat 输出用的 mtype 名字表，索引对齐 msg_type 枚举
static const char *_mtype_names[MSG_TYPE_ALL] = {
    "NONE", "STARTUP", "CLOSING", "TIMEOUT", "ACCEPT", "CONNECT",
    "SSLEXCHANGED", "HANDSHAKED", "RECV", "SEND", "CLOSE", "RECVFROM",
    "REQUEST", "RESPONSE", "FORK"
};

const char *_message_str(msg_type type) {
    if (type >= MSG_TYPE_NONE && type < MSG_TYPE_ALL) {
        return _mtype_names[type];
    }
    return "";
}
// 比较 seri string item 与字面命令名；seri string 零拷贝、非 NUL 结尾，须按 len 比较
static int32_t _debug_cmd_eq(const seri_item *item, const char *name, size_t nlen) {
    return nlen == item->v.s.len && 0 == memcmp(item->v.s.p, name, nlen);
}
// 回复纯文本响应给请求方；debug 响应一律 ERR_OK，结果(含错误说明)由文本承载，
// 否则 srey.request 对 erro!=ERR_OK 吞 data，文本到不了调用方。copy=1 内部拷贝。
static void _debug_resp(task_ctx *task, name_t src, uint64_t sess, const char *text, size_t len) {
    task_ctx *dtask = task_grab(task->loader, src);
    if (NULL == dtask) {
        return;
    }
    task_response(dtask, sess, ERR_OK, (void *)text, len, 1);
    task_ungrab(dtask);
}
// stat：拼各 mtype 的 nmsg / dispatch_cpu_ns / avg，格式与 Lua 端 stat 命令一致
static void _debug_stat(task_ctx *task, name_t src, uint64_t sess) {
    uint64_t nmsg[MSG_TYPE_ALL];
    uint64_t cpu[MSG_TYPE_ALL];
    task_stat(task, nmsg, cpu);
    binary_ctx bw;
    binary_init(&bw, NULL, 0, 0);
    binary_set_va(&bw, "%-14s %12s %18s %14s\n", "MTYPE", "NMSG", "DISPATCH_CPU_NS", "AVG_NS");
    uint64_t tnmsg = 0;
    uint64_t tcpu = 0;
    int32_t i;
    for (i = 0; i < MSG_TYPE_ALL; i++) {
        if (0 == nmsg[i]) {
            continue;
        }
        binary_set_va(&bw, "%-14s %12" PRIu64 " %18" PRIu64 " %14" PRIu64 "\n",
            _mtype_names[i], nmsg[i], cpu[i], cpu[i] / nmsg[i]);
        tnmsg += nmsg[i];
        tcpu += cpu[i];
    }
    binary_set_va(&bw, "%-14s %12" PRIu64 " %18" PRIu64 " %14" PRIu64,
        "TOTAL", tnmsg, tcpu, tnmsg > 0 ? tcpu / tnmsg : 0);
    _debug_resp(task, src, sess, bw.data, bw.offset);
    binary_free(&bw);
}
// C task 的 REQ_DEBUG 处理：seri 位置化解码（首元素 cmd 字符串）。
// 公共命令 stat/coros/loglv 处理；mem/gc/inject/hotfix 等 Lua VM 专属回 "not supported"；
// 二者均接管返回 ERR_OK。非公共命令(业务自定义)返回 ERR_FAILED 透传业务 on_requested。
int32_t _debug_request(task_ctx *task, message_ctx *msg) {
    seri_iter it;
    seri_iter_init(&it, msg->data, msg->size);
    seri_item cmd;
    if (1 != seri_iter_next(&it, &cmd) || SERI_ITEM_STRING != cmd.type) {
        // 非位置化 seri(业务自定义格式)：透传业务自行解析
        return ERR_FAILED;
    }
    // 公共命令处理后返回 ERR_OK
    if (_debug_cmd_eq(&cmd, "stat", 4)) {
        _debug_stat(task, msg->src, msg->sess);
        return ERR_OK;
    }
    if (_debug_cmd_eq(&cmd, "coros", 5)) {
        // coro_dump 返回挂起协程文本(C 协程无栈回溯,仅 sess/mtype/age);非协程 task 返 NULL
        size_t dlen = 0;
        char *dump = coro_dump(task, &dlen);
        if (NULL != dump) {
            _debug_resp(task, msg->src, msg->sess, dump, dlen);
            FREE(dump);
        } else {
            _debug_resp(task, msg->src, msg->sess, "coros: not a coroutine task.", strlen("coros: not a coroutine task."));
        }
        return ERR_OK;
    }
    if (_debug_cmd_eq(&cmd, "loglv", 5)) {
        seri_item lv;
        if (1 != seri_iter_next(&it, &lv) || SERI_ITEM_INT != lv.type) {
            // 命令已识别但参数错：回错误响应并接管，不透传
            _debug_resp(task, msg->src, msg->sess, "loglv: missing level.", strlen("loglv: missing level."));
            return ERR_OK;
        }
        log_setlv((LOG_LEVEL)lv.v.i);
        char buf[32];
        int32_t n = SNPRINTF(buf, sizeof(buf), "log level => %d", (int32_t)lv.v.i);
        _debug_resp(task, msg->src, msg->sess, buf, (size_t)n);
        return ERR_OK;
    }
    // mem/gc/inject/hotfix 是 Lua VM 专属公共命令,C task 无法执行：回 not supported 并接管,
    // 避免透传到无 on_requested 的框架 task 显示误导的 "unavailable"
    if (_debug_cmd_eq(&cmd, "mem", 3) || _debug_cmd_eq(&cmd, "gc", 2)
        || _debug_cmd_eq(&cmd, "inject", 6) || _debug_cmd_eq(&cmd, "hotfix", 6)) {
        _debug_resp(task, msg->src, msg->sess, "command not supported in C task.", strlen("command not supported in C task."));
        return ERR_OK;
    }
    // 非公共命令(业务自定义)：返回 ERR_FAILED 透传给业务 on_requested 自行处理
    return ERR_FAILED;
}
