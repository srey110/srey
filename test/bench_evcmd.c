#include "bench_evcmd.h"
#include "lib.h"
#if !defined(OS_WIN)
#include <unistd.h>
#include <poll.h>
#include "thread/thread.h"
#include "thread/spinlock.h"
#include "containers/queue.h"
#include "containers/fsqu.h"

// 固定总命令数(各档生产者横向可比)
#define EC_TOTAL 1600000
// 生产者数组上限
#define EC_MAXPROD 8

// 模拟 event 的 cmd_ctx(约 40 字节)
typedef struct ec_cmd {
    int32_t cmd;
    int64_t fd;
    int64_t len;
    int64_t skid;
    int64_t arg;
}ec_cmd;

// ── 方式 A:pipe 直接发数据 ───────────────────────────────────────────
typedef struct eca_arg {
    int fd_w;
    int32_t n;
}eca_arg;
static void _eca_producer(void *ud) {
    eca_arg *a = (eca_arg *)ud;
    ec_cmd c;
    ZERO(&c, sizeof(c));
    c.cmd = 1;
    for (int32_t i = 0; i < a->n; i++) {
        const char *p = (const char *)&c;
        size_t left = sizeof(c);
        while (left > 0) {
            ssize_t w = write(a->fd_w, p, left);
            if (w > 0) {
                p += w;
                left -= (size_t)w;
            }
        }
    }
}
typedef struct eca_carg {
    int fd_r;
    int32_t total;
}eca_carg;
static void _eca_consumer(void *ud) {
    eca_carg *a = (eca_carg *)ud;
    char buf[64 * sizeof(ec_cmd)];
    size_t acc = 0;
    int32_t got = 0;
    while (got < a->total) {
        ssize_t r = read(a->fd_r, buf + acc, sizeof(buf) - acc);
        if (r <= 0) {
            continue;
        }
        acc += (size_t)r;
        size_t ncmd = acc / sizeof(ec_cmd);
        got += (int32_t)ncmd;
        acc -= ncmd * sizeof(ec_cmd);
        if (acc > 0) {
            memmove(buf, buf + ncmd * sizeof(ec_cmd), acc);
        }
    }
}
static uint64_t _bench_evcmd_pipe(int32_t nprod, int32_t per) {
    int fds[2];
    if (0 != pipe(fds)) {
        return 0;
    }
    eca_arg pargs[EC_MAXPROD];
    pthread_t pths[EC_MAXPROD];
    eca_carg carg;
    pthread_t cth;
    carg.fd_r = fds[0];
    carg.total = nprod * per;
    for (int32_t i = 0; i < nprod; i++) {
        pargs[i].fd_w = fds[1];
        pargs[i].n = per;
    }
    uint64_t t0 = nowms();
    cth = thread_creat(_eca_consumer, &carg);
    for (int32_t i = 0; i < nprod; i++) {
        pths[i] = thread_creat(_eca_producer, &pargs[i]);
    }
    for (int32_t i = 0; i < nprod; i++) {
        thread_join(pths[i]);
    }
    thread_join(cth);
    uint64_t cost = nowms() - t0;
    close(fds[0]);
    close(fds[1]);
    return cost;
}
// ── 方式 B:queue+spinlock 存数据 + pipe 合并信号触发 ─────────────────
typedef struct ecb_shared {
    queue_ctx qu;
    spin_ctx lock;
    atomic_t pending;   // 0=无待处理信号,1=已触发(合并:仅 0→1 时 write 一次)
    int fd_r;
    int fd_w;
}ecb_shared;
typedef struct ecb_arg {
    ecb_shared *sh;
    int32_t n;
}ecb_arg;
static void _ecb_producer(void *ud) {
    ecb_arg *a = (ecb_arg *)ud;
    ec_cmd c;
    ZERO(&c, sizeof(c));
    c.cmd = 1;
    char b = 1;
    for (int32_t i = 0; i < a->n; i++) {
        spin_lock(&a->sh->lock);
        queue_push(&a->sh->qu, &c);
        spin_unlock(&a->sh->lock);
        if (ATOMIC_CAS(&a->sh->pending, 0, 1)) {
            while (write(a->sh->fd_w, &b, 1) <= 0) {}
        }
    }
}
typedef struct ecb_carg {
    ecb_shared *sh;
    int32_t total;
}ecb_carg;
static void _ecb_consumer(void *ud) {
    ecb_carg *a = (ecb_carg *)ud;
    char sig[256];
    int32_t got = 0;
    void *p;
    while (got < a->total) {
        if (read(a->sh->fd_r, sig, sizeof(sig)) <= 0) {
            continue;
        }
        ATOMIC_SET(&a->sh->pending, 0);
        for (;;) {
            spin_lock(&a->sh->lock);
            p = queue_pop(&a->sh->qu);
            spin_unlock(&a->sh->lock);
            if (NULL == p) {
                break;
            }
            got++;
        }
    }
}
static uint64_t _bench_evcmd_queue(int32_t nprod, int32_t per) {
    int fds[2];
    if (0 != pipe(fds)) {
        return 0;
    }
    ecb_shared sh;
    queue_init(&sh.qu, sizeof(ec_cmd), (uint32_t)(nprod * per));
    spin_init(&sh.lock, 0);
    ATOMIC_SET(&sh.pending, 0);
    sh.fd_r = fds[0];
    sh.fd_w = fds[1];
    ecb_arg pargs[EC_MAXPROD];
    pthread_t pths[EC_MAXPROD];
    ecb_carg carg;
    pthread_t cth;
    carg.sh = &sh;
    carg.total = nprod * per;
    for (int32_t i = 0; i < nprod; i++) {
        pargs[i].sh = &sh;
        pargs[i].n = per;
    }
    uint64_t t0 = nowms();
    cth = thread_creat(_ecb_consumer, &carg);
    for (int32_t i = 0; i < nprod; i++) {
        pths[i] = thread_creat(_ecb_producer, &pargs[i]);
    }
    for (int32_t i = 0; i < nprod; i++) {
        thread_join(pths[i]);
    }
    thread_join(cth);
    uint64_t cost = nowms() - t0;
    close(fds[0]);
    close(fds[1]);
    queue_free(&sh.qu);
    spin_free(&sh.lock);
    return cost;
}
// ── 方式 C:fsqu 存数据 + pipe 信号触发 ───────────────────────────────
// 即当前生产实现(_send_cmd:命令进 fsqu + pipe 传触发字节):Linux/Windows 下 fsqu 内部
// 是 mpq 无锁队列,macOS/BSD 下退化为 queue+spin(与方式 B 等价)。
// 方式 A(pipe 直写命令)已废弃,保留两档仅为留存选型依据。
// coalesce=0:每条命令写一次触发字节(与 _send_cmd 一致);
// coalesce=1:仅队列空→非空写一次(信号合并,实测多生产者下反而更慢,未采用)
typedef struct ecc_shared {
    fsqu_ctx qu;
    atomic_t pending;
    int32_t coalesce;
    int fd_r;
    int fd_w;
}ecc_shared;
typedef struct ecc_arg {
    ecc_shared *sh;
    int32_t n;
}ecc_arg;
static void _ecc_producer(void *ud) {
    ecc_arg *a = (ecc_arg *)ud;
    ec_cmd c;
    ZERO(&c, sizeof(c));
    c.cmd = 1;
    char b = 1;
    for (int32_t i = 0; i < a->n; i++) {
        fsqu_push(&a->sh->qu, &c);
        // coalesce=0 时短路跳过 CAS,每条都写触发字节
        if (0 == a->sh->coalesce
            || ATOMIC_CAS(&a->sh->pending, 0, 1)) {
            while (write(a->sh->fd_w, &b, 1) <= 0) {}
        }
    }
}
typedef struct ecc_carg {
    ecc_shared *sh;
    int32_t total;
}ecc_carg;
static void _ecc_consumer(void *ud) {
    ecc_carg *a = (ecc_carg *)ud;
    char sig[4 * ONEK];
    ec_cmd batch[64];
    int32_t got = 0;
    uint32_t n;
    struct pollfd pfd;
    pfd.fd = a->sh->fd_r;
    pfd.events = POLLIN;
    while (got < a->total) {
        // 抽干触发字节(同 _uev_cmd_run):每轮只读一次会让 64KB 管道在 coalesce=0 下持续饱和,
        // 生产者常态阻塞在 write。poll 的 1ms 超时兼作等待,免去忙等
        pfd.revents = 0;
        while (poll(&pfd, 1, 1) > 0) {
            if (read(a->sh->fd_r, sig, sizeof(sig)) <= 0) {
                break;
            }
        }
        ATOMIC_SET(&a->sh->pending, 0);
        for (;;) {
            n = fsqu_pop_sc_batch(&a->sh->qu, batch, (uint32_t)(sizeof(batch) / sizeof(batch[0])));
            if (0 == n) {
                break;
            }
            got += (int32_t)n;
        }
    }
    // 收尾必须再抽一次:生产者的触发字节写在 push 之后,最后一批 push 虽已计入 got,
    // 其 write 仍可能阻塞在满管道上,不排空会让 thread_join(生产者)永久卡死
    pfd.revents = 0;
    while (poll(&pfd, 1, 10) > 0) {
        if (read(a->sh->fd_r, sig, sizeof(sig)) <= 0) {
            break;
        }
    }
}
static uint64_t _bench_evcmd_fsqu(int32_t nprod, int32_t per, int32_t coalesce) {
    int fds[2];
    if (0 != pipe(fds)) {
        return 0;
    }
    ecc_shared sh;
    // 容量与 _uev_new_pipe 一致,超出即走 fsqu 内部溢出降级
    fsqu_init(&sh.qu, sizeof(ec_cmd), 4 * ONEK);
    ATOMIC_SET(&sh.pending, 0);
    sh.coalesce = coalesce;
    sh.fd_r = fds[0];
    sh.fd_w = fds[1];
    ecc_arg pargs[EC_MAXPROD];
    pthread_t pths[EC_MAXPROD];
    ecc_carg carg;
    pthread_t cth;
    carg.sh = &sh;
    carg.total = nprod * per;
    for (int32_t i = 0; i < nprod; i++) {
        pargs[i].sh = &sh;
        pargs[i].n = per;
    }
    uint64_t t0 = nowms();
    cth = thread_creat(_ecc_consumer, &carg);
    for (int32_t i = 0; i < nprod; i++) {
        pths[i] = thread_creat(_ecc_producer, &pargs[i]);
    }
    for (int32_t i = 0; i < nprod; i++) {
        thread_join(pths[i]);
    }
    thread_join(cth);
    uint64_t cost = nowms() - t0;
    close(fds[0]);
    close(fds[1]);
    fsqu_free(&sh.qu);
    return cost;
}
void bench_evcmd(void) {
    int32_t prods[] = { 1, 2, 4, 8 };
    int32_t np = (int32_t)(sizeof(prods) / sizeof(prods[0]));
    int32_t k;
    // 注:ec_cmd 40 字节 < 真实 cmd_ctx 约 72 字节,故 pipe 可容纳的命令数偏乐观
    // (64KB/40≈1638 vs 64KB/72≈910),方式 A 的实际表现会比此处略差
    LOG_INFO("[bench_evcmd] === A:pipe-direct  B:queue+spin  C:fsqu(=mpq on linux), total=%d ===", EC_TOTAL);
    for (k = 0; k < np; k++) {
        int32_t nprod = prods[k];
        int32_t per = EC_TOTAL / nprod;
        uint64_t a_ms = _bench_evcmd_pipe(nprod, per);
        uint64_t b_ms = _bench_evcmd_queue(nprod, per);
        uint64_t c_ms = _bench_evcmd_fsqu(nprod, per, 0);
        uint64_t d_ms = _bench_evcmd_fsqu(nprod, per, 1);
        LOG_INFO("[bench_evcmd] producers=%d A(pipe)=%llums B(queue+spin,coalesce)=%llums "
                 "C(fsqu,signal-per-cmd)=%llums D(fsqu,coalesce)=%llums | C/A=%.2fx D/A=%.2fx",
                 nprod, (unsigned long long)a_ms, (unsigned long long)b_ms,
                 (unsigned long long)c_ms, (unsigned long long)d_ms,
                 (a_ms > 0) ? (double)c_ms / (double)a_ms : 0.0,
                 (a_ms > 0) ? (double)d_ms / (double)a_ms : 0.0);
    }
}
#else
void bench_evcmd(void) {}
#endif
