#include "task_udp_multicast.h"

#define MCAST_GROUP "239.99.99.99"
#define UNI_MSG     "UNI_HELLO"
#define UNI_LEN     9

typedef struct task_udp_multicast_args {
    int32_t *ok;
    uint16_t port;
}task_udp_multicast_args;

static atomic_t _recv_count;

// 单播自收回调：验证 task_recvedfrom 路径基本工作
static void _net_recvfrom(task_ctx *task, SOCKET fd, uint64_t skid,
                          char ip[IP_LENS], uint16_t port, void *data, size_t size) {
    (void)task; (void)fd; (void)skid; (void)ip; (void)port;
    if (UNI_LEN == size && 0 == memcmp(data, UNI_MSG, UNI_LEN)) {
        ATOMIC_ADD(&_recv_count, 1);
    }
}

static void _startup(task_ctx *task) {
    task_udp_multicast_args *arg = (task_udp_multicast_args *)coro_get_arg(task);
    ATOMIC_SET(&_recv_count, 0);
    task_recvedfrom(task, _net_recvfrom);
    SOCKET fd;
    uint64_t skid;
    if (ERR_OK != task_udp(task, "0.0.0.0", arg->port, &fd, &skid)) {
        LOG_ERROR("udp_multicast: task_udp failed.");
        return;
    }
    ev_ctx *ev = &task->loader->netev;
    // 多播 4 API 路径验证：调用应一律 ERR_OK 投递成功(实际 setsockopt 在事件线程执行)
    if (ERR_OK != ev_udp_ttl(ev, fd, skid, 1)) {
        LOG_ERROR("udp_multicast: ev_udp_ttl post failed.");
        return;
    }
    if (ERR_OK != ev_udp_loop(ev, fd, skid, 1)) {
        LOG_ERROR("udp_multicast: ev_udp_loop post failed.");
        return;
    }
    if (ERR_OK != ev_udp_join(ev, fd, skid, MCAST_GROUP, NULL)) {
        LOG_ERROR("udp_multicast: ev_udp_join post failed.");
        return;
    }
    // 等 4 个 cmd 投递到事件线程并执行 setsockopt(若 setsockopt 失败 _on_cmd_udp_opt 内 LOG_ERROR)
    coro_sleep(task, 200);
    // 单播自收验证 task_recvedfrom 基础路径(127.0.0.1:port → 同 socket recvfrom)；
    // 多播实际 loopback 跨 OS/网络环境差异大,本测试只验证 API 调用路径不验证多播传输
    char *payload;
    MALLOC(payload, UNI_LEN);
    memcpy(payload, UNI_MSG, UNI_LEN);
    if (ERR_OK != ev_sendto(ev, fd, skid, "127.0.0.1", arg->port, payload, UNI_LEN, 0)) {
        LOG_ERROR("udp_multicast: ev_sendto unicast failed.");
        return;
    }
    int32_t poll;
    for (poll = 0; poll < 40; poll++) {
        coro_sleep(task, 50);
        if (ATOMIC_GET(&_recv_count) >= 1) {
            break;
        }
    }
    int32_t got = (int32_t)ATOMIC_GET(&_recv_count);
    if (got < 1) {
        LOG_ERROR("udp_multicast: unicast self-recv failed (%d/1).", got);
        return;
    }
    // 验证 leave API 路径
    if (ERR_OK != ev_udp_leave(ev, fd, skid, MCAST_GROUP, NULL)) {
        LOG_ERROR("udp_multicast: ev_udp_leave post failed.");
        return;
    }
    coro_sleep(task, 50);
    *(arg->ok) = 1;
    LOG_INFO("udp_multicast tested: 4 API paths OK + recvfrom path verified.");
}

void task_udp_multicast_start(loader_ctx *loader, const char *name, uint16_t port, int32_t *ok) {
    if (NULL == ok) {
        return;
    }
    task_udp_multicast_args *arg;
    CALLOC(arg, 1, sizeof(task_udp_multicast_args));
    arg->ok = ok;
    arg->port = port;
    coro_task_register(loader, name, 0, _startup, NULL, _free, arg);
}
