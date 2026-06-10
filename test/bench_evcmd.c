#include "bench_evcmd.h"
#include "lib.h"
#if !defined(OS_WIN)
#include <unistd.h>
#include "thread/thread.h"
#include "thread/spinlock.h"
#include "containers/queue.h"

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
void bench_evcmd(void) {
    int32_t prods[] = { 1, 2, 4, 8 };
    int32_t np = (int32_t)(sizeof(prods) / sizeof(prods[0]));
    int32_t k;
    LOG_INFO("[bench_evcmd] === pipe-direct vs queue+spin(pipe-signal), total=%d ===", EC_TOTAL);
    for (k = 0; k < np; k++) {
        int32_t nprod = prods[k];
        int32_t per = EC_TOTAL / nprod;
        uint64_t a_ms = _bench_evcmd_pipe(nprod, per);
        uint64_t b_ms = _bench_evcmd_queue(nprod, per);
        double sp = (b_ms > 0) ? (double)a_ms / (double)b_ms : 0.0;
        LOG_INFO("[bench_evcmd] producers=%d pipe-direct=%llums queue+spin=%llums queue-speedup=%.2fx",
                 nprod, (unsigned long long)a_ms, (unsigned long long)b_ms, sp);
    }
}
#else
void bench_evcmd(void) {}
#endif
