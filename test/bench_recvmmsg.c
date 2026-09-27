#include "bench_recvmmsg.h"
#include "lib.h"
#include "thread/thread.h"
#include "utils/timer.h"

// 逐个收(recvmsg)对批量收(recvmmsg，每次最多 RB_BATCH 个)。每个读事件里的收法照 srey 事件层：
// kqueue 平台按 kevent.data(排队负载字节)读够即停，其余平台(epoll/evport/devpoll/pollset)读到 EAGAIN 为止；
// 就绪等待在 kqueue 平台用 kqueue、其余用 poll，只替代事件后端，不改收包循环与计费方式。
// 负载：pp 一问一答(每个读事件 1 包)；b32/b256 客户端连发 N 包、服务端收到末包回 1 个 ack(每个读事件多包)。
// 判定按轮配对：每轮两种收法紧挨着跑，逐轮算 批量/逐个 的比值，取中位数，并数有几轮同向。中位数超出 RB_TOLER
// 且至少 RB_AGREE 轮同向才算真有差别，免得噪声带里来回摆的比值把结论翻来翻去。一问一答(墙钟或服务端 CPU)
// 或任一档突发(服务端 CPU，两档各判)真慢了就不该开；有一档突发 CPU 真省了、一问一答又不慢才开；其余保持逐个收。突发的墙钟只打印
// 不参与判定：回环上它主要看客户端发包节奏与 ack 落在哪一包，不代表收包本身的开销(srey 事件层实测 Linux 突发墙钟变快)
// "收包调用数"只数本程序发起的调用：libc 模拟的 recvmmsg(如 FreeBSD)内部逐个 recvmsg，这里看不到，
// 真实系统调用数要拿 strace -c / truss -c 跑一遍对照(调用里出现的是 recvmmsg 还是只有 recvmsg)
#define RB_MS       500 // 每项跑多久(毫秒)
#define RB_ROUNDS   9   // 两种收法逐轮换先后交错跑，取中位数
#define RB_BATCH    8   // 同 uev.h 的 UDP_RECV_BATCH
#define RB_PKT      64
#define RB_RBUF     65536 // 每块接收缓冲，同 MAX_RECVFROM_SIZE
#define RB_SOCKBUF  (4 * 1024 * 1024)
#define RB_TOLER    0.03
#define RB_AGREE    7   // RB_ROUNDS 轮里至少这么多轮同向才算数
#define RB_NCASE    3
#define RB_ACK      0x41434b21u

#if !defined(OS_WIN) && defined(MSG_WAITFORONE)
#if defined(EV_KQUEUE)
    #include <sys/event.h>
    #define RB_READINESS "kqueue (stop after kevent.data bytes)"
#else
    #include <poll.h>
    #define RB_READINESS "poll (read until EAGAIN)"
#endif

typedef struct rb_hdr {
    uint32_t seq;   // 第几轮
    uint32_t idx;   // 本轮第几包
    uint32_t cnt;   // 本轮总包数，1 为一问一答
    uint32_t magic;
}rb_hdr;
typedef struct rb_srv {
    SOCKET fd;
    int32_t batch;          // 0 逐个收，否则每次 recvmmsg 最多取这么多
    atomic_t stop;
    uint64_t calls;         // 发起的收包调用数(含读空那次)
    uint64_t pkts;
    uint64_t cpu_ns;        // 服务端线程 CPU
    char *bufs;
}rb_srv;
typedef struct rb_res {
    double wall;    // 每包墙钟 ns(一问一答为每个来回)
    double cpu;     // 服务端每包 CPU ns
    double calls;   // 服务端每包收包调用数
    uint64_t lost;
}rb_res;

static const char *_rb_name[RB_NCASE] = { "pp", "b32", "b256" };
static const int32_t _rb_burst[RB_NCASE] = { 1, 32, 256 };

// 收到一包：一问一答原样回，突发收到末包回 ack
static void _rb_handle(rb_srv *s, const char *data, size_t lens, const struct sockaddr_in *from) {
    rb_hdr h;
    s->pkts++;
    if (lens < sizeof(h)) {
        return;
    }
    memcpy(&h, data, sizeof(h));
    if (1 == h.cnt) {
        sendto(s->fd, data, lens, 0, (const struct sockaddr *)from, sizeof(*from));
        return;
    }
    if (h.idx + 1 == h.cnt) {
        h.magic = RB_ACK;
        sendto(s->fd, (char *)&h, sizeof(h), 0, (const struct sockaddr *)from, sizeof(*from));
    }
}
// 逐个收：want > 0 时读够这么多负载字节即停，否则读到 EAGAIN
static void _rb_recv_one(rb_srv *s, long want) {
    long got = 0;
    ssize_t n;
    struct sockaddr_in from;
    struct iovec iov;
    struct msghdr msg;
    iov.iov_base = s->bufs;
    iov.iov_len = RB_RBUF;
    for (;;) {
        ZERO(&msg, sizeof(msg));
        msg.msg_name = &from;
        msg.msg_namelen = sizeof(from);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        n = recvmsg(s->fd, &msg, 0);
        s->calls++;
        if (n < 0) {
            ASSERTAB(ERR_RW_RETRIABLE(ERRNO), "recvmsg failed.");
            return;
        }
        _rb_handle(s, s->bufs, (size_t)n, &from);
        got += (long)n;
        if (want > 0 && got >= want) {
            return;
        }
    }
}
// 批量收：停的条件同 _rb_recv_one
static void _rb_recv_batch(rb_srv *s, long want) {
    long got = 0;
    int32_t i, n;
    struct mmsghdr vec[RB_BATCH];
    struct iovec iov[RB_BATCH];
    struct sockaddr_in from[RB_BATCH];
    ZERO(vec, sizeof(vec));
    for (i = 0; i < s->batch; i++) {
        iov[i].iov_base = s->bufs + (size_t)i * RB_RBUF;
        iov[i].iov_len = RB_RBUF;
        vec[i].msg_hdr.msg_name = &from[i];
        vec[i].msg_hdr.msg_iov = &iov[i];
        vec[i].msg_hdr.msg_iovlen = 1;
    }
    for (;;) {
        for (i = 0; i < s->batch; i++) {
            vec[i].msg_hdr.msg_namelen = sizeof(from[i]);
        }
        n = recvmmsg(s->fd, vec, (unsigned int)s->batch, 0, NULL);
        s->calls++;
        if (n < 0) {
            ASSERTAB(ERR_RW_RETRIABLE(ERRNO), "recvmmsg failed.");
            return;
        }
        for (i = 0; i < n; i++) {
            _rb_handle(s, s->bufs + (size_t)i * RB_RBUF, (size_t)vec[i].msg_len, &from[i]);
            got += (long)vec[i].msg_len;
        }
        if (want > 0 && got >= want) {
            return;
        }
    }
}
static void _rb_on_readable(rb_srv *s, long want) {
    if (0 == s->batch) {
        _rb_recv_one(s, want);
    } else {
        _rb_recv_batch(s, want);
    }
}
static void _rb_srv_loop(void *arg) {
    rb_srv *s = arg;
    uint64_t c0 = timer_thread_cpu_ns();
#if defined(EV_KQUEUE)
    int kq = kqueue();
    struct kevent ev;
    struct timespec to = { 0, 100 * 1000000 };
    ASSERTAB(kq >= 0, "kqueue failed.");
    EV_SET(&ev, s->fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    ASSERTAB(kevent(kq, &ev, 1, NULL, 0, NULL) >= 0, "kevent add failed.");
    while (0 == ATOMIC_GET(&s->stop)) {
        if (kevent(kq, NULL, 0, &ev, 1, &to) > 0) {
            _rb_on_readable(s, (long)ev.data);
        }
    }
    close(kq);
#else
    struct pollfd pfd;
    pfd.fd = s->fd;
    pfd.events = POLLIN;
    while (0 == ATOMIC_GET(&s->stop)) {
        pfd.revents = 0;
        if (poll(&pfd, 1, 100) > 0) {
            _rb_on_readable(s, 0);
        }
    }
#endif
    s->cpu_ns = timer_thread_cpu_ns() - c0;
}
static SOCKET _rb_socket(int32_t nonblock) {
    int32_t bufsz = RB_SOCKBUF;
    SOCKET fd = sock_create_cloexec(AF_INET, SOCK_DGRAM, 0, nonblock);
    ASSERTAB(INVALID_SOCK != fd, "socket failed.");
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (char *)&bufsz, sizeof(bufsz));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (char *)&bufsz, sizeof(bufsz));
    return fd;
}
// 跑一项：batch 为 0 逐个收，否则批量收
static rb_res _rb_run(int32_t burst, int32_t batch) {
    rb_res res;
    rb_srv s;
    pthread_t th;
    timer_ctx tm;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    struct timeval rto = { 1, 0 };
    char pkt[RB_PKT];
    rb_hdr h, ack;
    uint64_t sent = 0, rounds = 0;
    SOCKET cli;
    int32_t i;
    ZERO(&s, sizeof(s));
    ZERO(&res, sizeof(res));
    s.batch = batch;
    MALLOC(s.bufs, (size_t)(0 == batch ? 1 : batch) * RB_RBUF);
    s.fd = _rb_socket(1);
    ZERO(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERTAB(0 == bind(s.fd, (struct sockaddr *)&addr, sizeof(addr))
             && 0 == getsockname(s.fd, (struct sockaddr *)&addr, &alen), "bind failed.");
    cli = _rb_socket(0);
    setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (char *)&rto, sizeof(rto));
    ASSERTAB(0 == connect(cli, (struct sockaddr *)&addr, sizeof(addr)), "connect failed.");
    memset(pkt, 'x', sizeof(pkt));
    th = thread_creat(_rb_srv_loop, &s);
    MSLEEP(20);
    timer_init(&tm);
    while (timer_elapsed_ms(&tm) < RB_MS) {
        for (i = 0; i < burst; i++) {
            h.seq = (uint32_t)rounds;
            h.idx = (uint32_t)i;
            h.cnt = (uint32_t)burst;
            h.magic = 0;
            memcpy(pkt, &h, sizeof(h));
            send(cli, pkt, sizeof(pkt), 0);
            sent++;
        }
        for (;;) {// 等本轮回包；超时算丢一轮，旧轮迟到的回包按 seq 跳过
            if (recv(cli, (char *)&ack, sizeof(ack), 0) < (ssize_t)sizeof(ack)) {
                res.lost++;
                break;
            }
            if (ack.seq == (uint32_t)rounds) {
                break;
            }
        }
        rounds++;
    }
    res.wall = (double)timer_elapsed(&tm) / (double)(1 == burst ? rounds : sent);
    ATOMIC_SET(&s.stop, 1);
    thread_join(th);
    CLOSE_SOCK(cli);
    CLOSE_SOCK(s.fd);
    FREE(s.bufs);
    res.cpu = (double)s.cpu_ns / (double)(0 == s.pkts ? 1 : s.pkts);
    res.calls = (double)s.calls / (double)(0 == s.pkts ? 1 : s.pkts);
    return res;
}
static int _rb_cmp(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
// 中位数(会打乱 v 的顺序)
static double _rb_median(double *v, int32_t n) {
    qsort(v, (size_t)n, sizeof(double), _rb_cmp);
    return 0 != n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}
// 逐轮配对比值的结论：1 真变慢，-1 真变快，0 分不出；med 带回中位数
static int32_t _rb_judge(const double *ratio, double *med) {
    double v[RB_ROUNDS];
    int32_t i, worse = 0, better = 0;
    for (i = 0; i < RB_ROUNDS; i++) {
        v[i] = ratio[i];
        worse += ratio[i] > 1.0;
        better += ratio[i] < 1.0;
    }
    *med = _rb_median(v, RB_ROUNDS);
    if (*med > 1.0 + RB_TOLER && worse >= RB_AGREE) {
        return 1;
    }
    if (*med < 1.0 - RB_TOLER && better >= RB_AGREE) {
        return -1;
    }
    return 0;
}
void bench_recvmmsg(void) {
    double raw[RB_NCASE][2][3][RB_ROUNDS];// [项][收法][墙钟/CPU/调用数][轮]
    double ratio[RB_NCASE][2][RB_ROUNDS];// [项][墙钟/CPU][轮] 批量/逐个
    double med[3], rmed[2], ppw, ppc, bc[2], bw;
    int32_t jw, jc, jb[2], r, c, m, k, mode;
    uint64_t lost[RB_NCASE][2];
    rb_res res;
    ZERO(lost, sizeof(lost));
    LOG_INFO("[bench_recvmmsg] %s, readiness %s, batch=%d, %d ms x %d rounds interleaved", EV_NAME, RB_READINESS,
             RB_BATCH, RB_MS, RB_ROUNDS);
    for (r = 0; r < RB_ROUNDS; r++) {
        for (c = 0; c < RB_NCASE; c++) {
            for (m = 0; m < 2; m++) {
                mode = 0 != (r + c) % 2 ? 1 - m : m;// 每轮每项换先后，抵消顺序漂移
                res = _rb_run(_rb_burst[c], 0 == mode ? 0 : RB_BATCH);
                raw[c][mode][0][r] = res.wall;
                raw[c][mode][1][r] = res.cpu;
                raw[c][mode][2][r] = res.calls;
                lost[c][mode] += res.lost;
            }
            ratio[c][0][r] = raw[c][1][0][r] / raw[c][0][0][r];
            ratio[c][1][r] = raw[c][1][1][r] / raw[c][0][1][r];
        }
    }
    LOG_INFO("[bench_recvmmsg] case | wall ns one/batch (paired ratio) | server cpu ns one/batch (paired ratio) | calls/pkt one/batch | lost");
    for (c = 0; c < RB_NCASE; c++) {
        for (k = 0; k < 2; k++) {
            _rb_judge(ratio[c][k], &rmed[k]);
        }
        for (m = 0; m < 2; m++) {
            for (k = 0; k < 3; k++) {
                med[k] = _rb_median(raw[c][m][k], RB_ROUNDS);
                raw[c][m][k][0] = med[k];// 中位数存回首格供下面打印
            }
        }
        LOG_INFO("[bench_recvmmsg] %-4s | %9.1f / %9.1f (%.3f) | %8.1f / %8.1f (%.3f) | %.3f / %.3f | %llu/%llu",
                 _rb_name[c], raw[c][0][0][0], raw[c][1][0][0], rmed[0], raw[c][0][1][0], raw[c][1][1][0], rmed[1],
                 raw[c][0][2][0], raw[c][1][2][0], (unsigned long long)lost[c][0], (unsigned long long)lost[c][1]);
    }
    jw = _rb_judge(ratio[0][0], &ppw);
    jc = _rb_judge(ratio[0][1], &ppc);
    jb[0] = _rb_judge(ratio[1][1], &bc[0]);
    jb[1] = _rb_judge(ratio[2][1], &bc[1]);
    _rb_judge(ratio[1][0], &rmed[0]);
    _rb_judge(ratio[2][0], &rmed[1]);
    bw = (rmed[0] + rmed[1]) / 2.0;
    if (1 == jw || 1 == jc) {
        LOG_INFO("[bench_recvmmsg] verdict: DISABLE, one-request-one-reply slower (wall %.3f cpu %.3f)", ppw, ppc);
    } else if (1 == jb[0] || 1 == jb[1]) {
        LOG_INFO("[bench_recvmmsg] verdict: DISABLE, bursts cost more server cpu (b32 %.3f b256 %.3f)", bc[0], bc[1]);
    } else if (-1 == jb[0] || -1 == jb[1]) {
        LOG_INFO("[bench_recvmmsg] verdict: ENABLE, bursts save server cpu (b32 %.3f b256 %.3f), pp holds (wall %.3f cpu %.3f)",
                 bc[0], bc[1], ppw, ppc);
    } else {
        LOG_INFO("[bench_recvmmsg] verdict: NEUTRAL, no consistent difference (pp wall %.3f cpu %.3f, burst cpu %.3f / %.3f)",
                 ppw, ppc, bc[0], bc[1]);
    }
    if (bw > 1.0 + RB_TOLER) {
        LOG_INFO("[bench_recvmmsg] note: burst wall %.3f is worse on this loopback probe, not used for the verdict", bw);
    }
    LOG_INFO("[bench_recvmmsg] ratio = batch/one paired per round; a difference counts only if the median is beyond %.0f%% and >= %d of %d rounds agree",
             RB_TOLER * 100, RB_AGREE, RB_ROUNDS);
    LOG_INFO("[bench_recvmmsg] calls/pkt counts only this program's calls; check real syscalls with strace -c / truss -c");
}
#else
void bench_recvmmsg(void) {
    LOG_INFO("[bench_recvmmsg] %s: no recvmmsg (MSG_WAITFORONE undefined), UDP reads one datagram per call, nothing to decide", EV_NAME);
}
#endif
