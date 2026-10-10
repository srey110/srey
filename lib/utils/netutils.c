#include "utils/netutils.h"
#include "utils/netaddr.h"

#define MSEC    1000 //毫秒与秒的换算系数
#ifdef OS_WIN
// 0:未初始化 1:初始化中或收尾中 >=2:就绪（引用数 = 值 - 1）。
// 只保证"启停各一次"与"init 期间并发调用"两种用法,不是通用的并发引用计数
static atomic_t _init_sock_ref = 0;
#endif

void sock_init(void) {
#ifdef OS_WIN
    for (;;) {
        if (ATOMIC_CAS(&_init_sock_ref, 0, 1)) {
            WSADATA wsdata;
            WORD ver = MAKEWORD(2, 2);
            ASSERTAB_CODE(WSAStartup(ver, &wsdata));
            ATOMIC_SET_RELEASE(&_init_sock_ref, 2);
            return;
        }
        if (ATOMIC_GET(&_init_sock_ref) >= 2) {
            ATOMIC_ADD(&_init_sock_ref, 1);
            return;
        }
        // 值为 1:别人正在 init,或 sock_clean 已减到 1 还没置 0。不能死等 >= 2
        // (状态 1 可能正走向 0),回头重试 CAS,对方置 0 后即可接手
        CPU_PAUSE();
    }
#endif
}
void sock_clean(void) {
#ifdef OS_WIN
    if (2 == ATOMIC_ADD(&_init_sock_ref, -1)) {
        (void)WSACleanup();
        ATOMIC_SET_RELEASE(&_init_sock_ref, 0);
    }
#endif
}
int32_t sock_nread(SOCKET fd) {
#if defined(OS_WIN)
    u_long nread = 0;
    if (ioctlsocket(fd, FIONREAD, &nread) < ERR_OK) {
        return ERR_FAILED;
    }
    return (int32_t)nread;
#else
    int32_t nread = 0;
    if (ioctl(fd, FIONREAD, &nread) < ERR_OK) {
        return ERR_FAILED;
    }
    return nread;
#endif
}
int32_t sock_error(SOCKET fd) {
    int32_t err;
    socklen_t len = (socklen_t)sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &len) < ERR_OK) {
        return ERR_FAILED;
    }
    return err;
}
int32_t sock_checkconn(SOCKET fd) {
#ifdef OS_WIN
    int32_t time;
    int32_t len = (int32_t)sizeof(time);
    if (getsockopt(fd, SOL_SOCKET, SO_CONNECT_TIME, (char *)&time, &len) < ERR_OK) {
        return ERR_FAILED;
    }
    return -1 == time ? ERR_FAILED : ERR_OK;
#else
    return sock_error(fd);
#endif
}
int32_t sock_type(SOCKET fd) {
    int32_t stype = 0;
    socklen_t len = (socklen_t)sizeof(stype);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, (char *)&stype, &len) < ERR_OK) {
        return ERR_FAILED;
    }
    return stype;
}
int32_t sock_family(SOCKET fd) {
#if defined(OS_WIN)
    WSAPROTOCOL_INFO info;
    int32_t lens = (int32_t)sizeof(info);
    if (getsockopt(fd, SOL_SOCKET, SO_PROTOCOL_INFO, (char *)&info, &lens) < ERR_OK) {
        return ERR_FAILED;
    }
    return info.iAddressFamily;
#else
#ifdef SO_DOMAIN
    int32_t family = 0;
    socklen_t lens = (socklen_t)sizeof(family);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &family, &lens) < 0) {
        return ERR_FAILED;
    }
    return family;
#else
    netaddr_ctx addr;
    if (ERR_OK != netaddr_local(&addr, fd)) {
        return ERR_FAILED;
    }
    return netaddr_family(&addr);
#endif
#endif
}
// setsockopt 设 bool=1 标志;成功 ERR_OK,失败 ERR_FAILED
static int32_t _setsockopt_flag(SOCKET fd, int32_t level, int32_t opt) {
    int32_t flag = 1;
    if (setsockopt(fd, level, opt, (char *)&flag, (int32_t)sizeof(flag)) < ERR_OK) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t sock_nodelay(SOCKET fd) {
    return _setsockopt_flag(fd, IPPROTO_TCP, TCP_NODELAY);
}
int32_t sock_nonblock(SOCKET fd) {
#if defined(OS_WIN)
    u_long flag = 1;
    if (ioctlsocket(fd, FIONBIO, &flag) < ERR_OK) {
        return ERR_FAILED;
    }
#elif defined(OS_LINUX) || defined(OS_DARWIN) || defined(OS_BSD)
    int32_t on = 1;
    if (ioctl(fd, FIONBIO, &on) < ERR_OK) {
        return ERR_FAILED;
    }
#else
    int32_t flag = fcntl(fd, F_GETFL, NULL);
    if (ERR_FAILED == flag) {
        return ERR_FAILED;
    }
    if (!(flag & O_NONBLOCK)) {
        if (ERR_FAILED == fcntl(fd, F_SETFL, flag | O_NONBLOCK)) {
            return ERR_FAILED;
        }
    }
#endif
    return ERR_OK;
}
int32_t sock_reuseaddr(SOCKET fd, int32_t istcp) {
#if defined(OS_WIN)
    if (0 != istcp) {
        return _setsockopt_flag(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE);
    }
#else
    (void)istcp;
#endif
    return _setsockopt_flag(fd, SOL_SOCKET, SO_REUSEADDR);
}
int32_t sock_reuseport(SOCKET fd) {
#ifdef SO_REUSEPORT_LB
    // FreeBSD 的 SO_REUSEPORT 只允许重复绑定,TCP 监听并不分发,要 _LB 才按连接派发到各 fd
    if (ERR_OK == _setsockopt_flag(fd, SOL_SOCKET, SO_REUSEPORT_LB)) {
        return ERR_OK;
    }
#endif
#ifdef SO_REUSEPORT
    return _setsockopt_flag(fd, SOL_SOCKET, SO_REUSEPORT);
#else
    return ERR_OK;
#endif
}
int32_t sock_v6only(SOCKET fd, int32_t family) {
    if (AF_INET6 != family) {
        return ERR_OK;
    }
#ifdef IPV6_V6ONLY
    return _setsockopt_flag(fd, IPPROTO_IPV6, IPV6_V6ONLY);
#else
    (void)fd;
    return ERR_OK;
#endif
}
int32_t sock_keepalive(SOCKET fd, const int32_t delay, const int32_t intvl) {
    if (ERR_OK != _setsockopt_flag(fd, SOL_SOCKET, SO_KEEPALIVE)) {
        return ERR_FAILED;
    }
    if (0 >= delay) {
        return ERR_OK;
    }
#if defined(OS_WIN)
    struct tcp_keepalive kpa;
    struct tcp_keepalive out;
    DWORD ret = 0;
    // 秒换毫秒：两个字段是 ULONG，按 64 位乘再封顶，秒数大时 int32 乘法会溢出
    uint64_t ms = (uint64_t)delay * MSEC;
    kpa.onoff = 1;
    kpa.keepalivetime = (ULONG)(ms > ULONG_MAX ? ULONG_MAX : ms);
    ms = (uint64_t)intvl * MSEC;
    kpa.keepaliveinterval = (ULONG)(ms > ULONG_MAX ? ULONG_MAX : ms);
    if (WSAIoctl(fd, SIO_KEEPALIVE_VALS, (LPVOID)&kpa, sizeof(struct tcp_keepalive),
        (LPVOID)&out, sizeof(struct tcp_keepalive), &ret, NULL, NULL) < ERR_OK) {
        return ERR_FAILED;
    }
#else
    //首次发送 keepalive 前的空闲等待秒数
#ifdef TCP_KEEPIDLE
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, (char *)&delay, (int32_t)sizeof(delay)) < ERR_OK) {
        return ERR_FAILED;
    }
#elif defined(TCP_KEEPALIVE) && !defined(OS_SUN)
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, (char *)&delay, (int32_t)sizeof(delay)) < ERR_OK) {
        return ERR_FAILED;
    }
#endif
    //keepalive 探测包发送间隔秒数
#ifdef TCP_KEEPINTVL
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, (char *)&intvl, (int32_t)sizeof(intvl)) < ERR_OK) {
        return ERR_FAILED;
    }
#else
    (void)intvl;
#endif
    //keepalive 最大重试次数
#ifdef TCP_KEEPCNT
    int32_t cnt = 3;
    if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, (char *)&cnt, (int32_t)sizeof(cnt)) < ERR_OK) {
        return ERR_FAILED;
    }
#endif
#endif
    return ERR_OK;
}
int32_t sock_linger(SOCKET fd) {
    struct linger lg = { 1, 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_LINGER, (char *)&lg, (int32_t)sizeof(lg)) < ERR_OK) {
        return ERR_FAILED;
    }
    return ERR_OK;
}
SOCKET sock_create_cloexec(int32_t family, int32_t type, int32_t proto, int32_t nonblock) {
#if defined(SOCK_CLOEXEC)
    // Linux/BSD/Solaris：并进 type 原子设置，无 create→设标志 的竞态窗口
    type |= SOCK_CLOEXEC;
#endif
#if defined(SOCK_NONBLOCK)
    // 有这个标志就跟 CLOEXEC 一道设完,省掉下面那次 fcntl;置 0 表示已办完
    if (0 != nonblock) {
        type |= SOCK_NONBLOCK;
        nonblock = 0;
    }
#endif
#if defined(OS_WIN)
    // WSASocket 建 overlapped(IOCP 必需) + 禁句柄被 CreateProcess 继承；proto=0 按 family/type 选默认(TCP/UDP)
    SOCKET fd = WSASocket(family, type, proto, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#else
    SOCKET fd = socket(family, type, proto);
#endif
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
#if !defined(SOCK_CLOEXEC) && !defined(OS_WIN)
    // macOS 等：无 SOCK_CLOEXEC，create 后 fcntl 兜底
    SET_CLOEXEC(fd);
#endif
    // 设失败必须关掉重报:调用方拿到个阻塞 fd 会在事件循环里挂死,比返回失败难查得多
    if (0 != nonblock
        && ERR_OK != sock_nonblock(fd)) {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
SOCKET sock_accept_cloexec(SOCKET fd, struct sockaddr *addr, socklen_t *addrlen, int32_t nonblock) {
#if defined(HAVE_ACCEPT4)
    // 有 accept4 就把非阻塞和 CLOEXEC 一次设完,省掉调用方那次 F_SETFL
    return accept4(fd, addr, addrlen, nonblock ? (SOCK_CLOEXEC | SOCK_NONBLOCK) : SOCK_CLOEXEC);
#else
    // macOS/Solaris/Windows：无 accept4，accept 后兜底设标志
    SOCKET nfd = accept(fd, addr, addrlen);
    if (INVALID_SOCK != nfd) {
        SET_CLOEXEC(nfd);
        // 设不上按 accept 失败处理:调用方不再补设,放行一个阻塞 fd 进事件循环是挂死不是报错。
        // 关之前存下错误码再还原——调用方要靠它分类(见 _usk_check_accept),CLOSE_SOCK 会覆盖它
        if (0 != nonblock
            && ERR_OK != sock_nonblock(nfd)) {
            int32_t err = ERRNO;
            CLOSE_SOCK(nfd);
#if defined(OS_WIN)
            SetLastError((DWORD)err);
#else
            errno = err;
#endif
            return INVALID_SOCK;
        }
    }
    return nfd;
#endif
}
// 在本地回环地址上创建并监听一个临时 TCP 套接字，供 sock_pair 使用
static SOCKET _sock_listen(void) {
    netaddr_ctx addr;
    if (ERR_OK != netaddr_set(&addr, "127.0.0.1", 0)) {
        return INVALID_SOCK;
    }
    // 建阻塞的:下面那次 accept 要同步等对端连上来
    SOCKET fd = sock_create_cloexec(AF_INET, SOCK_STREAM, 0, 0);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    if (ERR_OK != bind(fd, netaddr_addr(&addr), netaddr_size(&addr))) {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    if (ERR_OK != listen(fd, 1)) {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
// 连接到指定地址，返回连接成功的套接字，供 sock_pair 使用
static SOCKET _sockcnt(union netaddr_ctx *paddr) {
    // 同样建阻塞的:下面是同步 connect,非阻塞会返 EINPROGRESS 被当成失败。
    // 要非阻塞由 sock_pair 连通后再转
    SOCKET fd = sock_create_cloexec(AF_INET, SOCK_STREAM, 0, 0);
    if (INVALID_SOCK == fd) {
        return INVALID_SOCK;
    }
    if (ERR_OK != connect(fd, netaddr_addr(paddr), netaddr_size(paddr))) {
        CLOSE_SOCK(fd);
        return INVALID_SOCK;
    }
    return fd;
}
int32_t sock_pair(SOCKET acSock[2], int32_t nonblock) {
    SOCKET fdlsn = _sock_listen();
    if (INVALID_SOCK == fdlsn) {
        return ERR_FAILED;
    }
    netaddr_ctx addr;
    if (ERR_OK != netaddr_local(&addr, fdlsn)) {
        CLOSE_SOCK(fdlsn);
        return ERR_FAILED;
    }
    SOCKET fdcn = _sockcnt(&addr);
    if (INVALID_SOCK == fdcn) {
        CLOSE_SOCK(fdlsn);
        return ERR_FAILED;
    }
    netaddr_ctx listen_addr;
    socklen_t addrlen = (socklen_t)sizeof(netaddr_ctx);
    SOCKET fdacp = sock_accept_cloexec(fdlsn, netaddr_addr(&listen_addr), &addrlen, nonblock);
    if (INVALID_SOCK == fdacp) {
        CLOSE_SOCK(fdlsn);
        CLOSE_SOCK(fdcn);
        return ERR_FAILED;
    }
    CLOSE_SOCK(fdlsn);
    if (ERR_OK != netaddr_local(&addr, fdcn)) {
        CLOSE_SOCK(fdacp);
        CLOSE_SOCK(fdcn);
        return ERR_FAILED;
    }
    if (ERR_OK != netaddr_compare(&listen_addr, &addr)) {
        CLOSE_SOCK(fdacp);
        CLOSE_SOCK(fdcn);
        return ERR_FAILED;
    }
    sock_nodelay(fdacp);
    sock_nodelay(fdcn);
    if (nonblock) {
        // fdacp 已由 sock_accept_cloexec 一并设好，这里只补主动连的那端
        sock_nonblock(fdcn);
    }
    acSock[0] = fdacp;
    acSock[1] = fdcn;
    return ERR_OK;
}
