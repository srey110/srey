#include "bench_weights.h"
#include "lib.h"
#include "srey/loader.h"
#include "srey/task.h"
#include "srey/spub.h"

// 并发 task 数：要多于最大 nworker，各档 weight 才都参与得进来
#define BW_NTASK   16
// 每个 task 预灌的消息条数。取这么大是为了把单轮耗时抬到毫秒计时有分辨率的量级——
// 2 万条时整轮只要 5ms,各 task 完成时刻全落在同一毫秒里,离差恒为 0 什么也看不出来
#define BW_NMSG    200000
// 每档重复轮数,按耗时排序取中位那轮。实测跨运行能差 2.7 倍(nworker=2 上 cur 有 57/152 两种
// 稳定态,取决于 16 个 task 初始落到哪个 worker),轮数少了会把某一态当成结论
#define BW_ROUNDS  5
// weights 槽位数,与 loader_init 里那张表一致
#define BW_NSLOT   32
// 单轮上限,卡住时不至于把整个 bench 挂死
#define BW_MAXMS   30000
// task_register 会投一条 STARTUP,等它消化完再灌正式负载
#define BW_WARMMS  100

typedef struct bw_stat {
    uint32_t nrecv;   // 已处理条数；同一 task 同时刻仅一个 worker 在跑，无需原子
    uint64_t t_done;  // 处理完最后一条的时刻
    char _pad[CACHELINE_SIZE];// 隔开相邻 task 的计数，免 false sharing
}bw_stat;
typedef struct bw_result {
    int32_t ndone;    // 本轮跑完的 task 数
    uint64_t cost;    // 整轮耗时(毫秒)
    uint64_t first;   // 最早完成的 task 相对开跑时刻(毫秒)
    uint64_t last;    // 最晚完成的
    uint64_t spread;  // last - first,越小越公平
}bw_result;
// 候选档位表。loader_init 里那张是编译期定死的,这里在 loader 起来后覆盖 worker->weight
// 来横向比——worker 线程此刻还没有任何消息可处理,覆盖是安全的
typedef struct bw_cfg {
    const char *name;
    int32_t w[BW_NSLOT];
}bw_cfg;

static const bw_cfg _bw_cfgs[] = {
    { "cur    3,-1, 1, 2", { 3,-1,1,2, 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 1,1,1,1, 2,2,2,2, 3,3,3,3 } },
    { "altB   3, 1, 2,-1", { 3,1,2,-1, 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 1,1,1,1, 2,2,2,2, 3,3,3,3 } },
    { "altF   3, 1,-1, 2", { 3,1,-1,2, 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 1,1,1,1, 2,2,2,2, 3,3,3,3 } },
    { "altG   3, 2,-1, 1", { 3,2,-1,1, 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 1,1,1,1, 2,2,2,2, 3,3,3,3 } },
    { "altI   3, 1, 2, 2 -1@8", { 3,1,2,2, 0,0,0,0, -1,1,1,1, 2,2,2,2, 3,3,3,3, 1,1,1,1, 2,2,2,2, 3,3,3,3 } },
};

static bw_stat _bw_stats[BW_NTASK];
static atomic_t _bw_ndone;

// 自定义分发：不跑业务只记账。消息不带 data，故无需清理
static void _bw_dispatch(task_dispatch_arg *arg) {
    if (MSG_TYPE_REQUEST != arg->msg->mtype) {
        // STARTUP / CLOSING 等框架消息必须交回默认处理,直接丢会让 task 关不掉、loader_free 死等
        _message_run(arg->task, arg->msg);
        return;
    }
    bw_stat *st = &_bw_stats[(uint32_t)arg->msg->sess];
    st->nrecv++;
    if (BW_NMSG == st->nrecv) {
        st->t_done = nowms();
        ATOMIC_ADD(&_bw_ndone, 1);
    }
}
// 跑一轮：起 loader、覆盖档位、建 task、预灌、放行、等完成
static void _bw_round(uint16_t nworker, const bw_cfg *cfg, bw_result *out) {
    task_ctx *tasks[BW_NTASK];
    message_ctx msg;
    int32_t i, k;
    uint64_t t0, t1, tmin, tmax;
    ZERO(_bw_stats, sizeof(_bw_stats));
    ATOMIC_SET(&_bw_ndone, 0);
    loader_ctx *loader = loader_init(1, nworker, 0);
    for (i = 0; i < (int32_t)nworker; i++) {
        loader->worker[i].weight = cfg->w[i % BW_NSLOT];
    }
    for (i = 0; i < BW_NTASK; i++) {
        tasks[i] = task_new(loader, NULL, BW_NMSG + 16, _bw_dispatch, NULL, NULL);
        task_register(tasks[i], NULL, NULL);
    }
    MSLEEP(BW_WARMMS);
    // 先全部入队不放行：开跑那一刻每个 task 的队列长度必须一致，
    // weight 是按 lens 取的，长度不齐结果没法横向比
    ZERO(&msg, sizeof(message_ctx));
    msg.mtype = MSG_TYPE_REQUEST;
    for (i = 0; i < BW_NTASK; i++) {
        msg.sess = (uint64_t)i;
        for (k = 0; k < BW_NMSG; k++) {
            _task_message_push(tasks[i], &msg);
        }
    }
    t0 = nowms();
    for (i = 0; i < BW_NTASK; i++) {
        _task_message_active(tasks[i]);
    }
    while ((int32_t)ATOMIC_GET(&_bw_ndone) < BW_NTASK
           && nowms() < t0 + BW_MAXMS) {
        MSLEEP(1);
    }
    t1 = nowms();
    tmin = t1;
    tmax = t0;
    for (i = 0; i < BW_NTASK; i++) {
        if (0 == _bw_stats[i].t_done) {
            continue;// 本轮没跑完的不参与离差统计
        }
        if (_bw_stats[i].t_done < tmin) {
            tmin = _bw_stats[i].t_done;
        }
        if (_bw_stats[i].t_done > tmax) {
            tmax = _bw_stats[i].t_done;
        }
    }
    out->ndone = (int32_t)ATOMIC_GET(&_bw_ndone);
    out->cost = (t1 > t0) ? (t1 - t0) : 1;
    out->first = tmin - t0;
    out->last = tmax - t0;
    out->spread = tmax - tmin;
    loader_free(loader);
}
// 按 cost 排序后取中位那轮的下标
static int32_t _bw_median_idx(const bw_result *r, int32_t n) {
    int32_t idx[BW_ROUNDS], i, j, t;
    for (i = 0; i < n; i++) {
        idx[i] = i;
    }
    for (i = 0; i < n - 1; i++) {
        for (j = 0; j < n - 1 - i; j++) {
            if (r[idx[j]].cost > r[idx[j + 1]].cost) {
                t = idx[j];
                idx[j] = idx[j + 1];
                idx[j + 1] = t;
            }
        }
    }
    return idx[n / 2];
}
void bench_weights(void) {
    static const uint16_t nws[] = { 1, 2, 4, 8, 16 };
    bw_result r[BW_ROUNDS];
    uint64_t total = (uint64_t)BW_NTASK * BW_NMSG;
    size_t n, c;
    int32_t i, mid;
    LOG_INFO("loader weight bench: %d tasks x %d msgs each, median of %d rounds; spread is fairness (lower better)",
             BW_NTASK, BW_NMSG, BW_ROUNDS);
    for (n = 0; n < ARRAY_SIZE(nws); n++) {
        for (c = 0; c < ARRAY_SIZE(_bw_cfgs); c++) {
            for (i = 0; i < BW_ROUNDS; i++) {
                _bw_round(nws[n], &_bw_cfgs[c], &r[i]);
            }
                mid = _bw_median_idx(r, BW_ROUNDS);
            LOG_INFO("nworker %2d | %s | %6llu k/s | spread %3llu | raw %llu/%llu/%llu/%llu/%llu ms",
                     (int32_t)nws[n], _bw_cfgs[c].name,
                     (unsigned long long)(total / r[mid].cost),
                     (unsigned long long)r[mid].spread,
                     (unsigned long long)r[0].cost, (unsigned long long)r[1].cost,
                     (unsigned long long)r[2].cost, (unsigned long long)r[3].cost,
                     (unsigned long long)r[4].cost);
        }
    }
}
