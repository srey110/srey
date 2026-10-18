#include "base/srand.h"
#include "base/err.h"
#include "base/macro_util.h"
#include "base/macro_atomic.h"
#include "base/macro_log.h"
#include "base/stime.h"

// Linux 优先 getrandom(2)；头文件太老（内核头 < 3.17）没有这个调用号时整段退到 /dev/urandom
#if defined(OS_LINUX) && defined(SYS_getrandom)
    #define CSPRNG_GETRANDOM 1
#else
    #define CSPRNG_GETRANDOM 0
#endif
#ifdef OS_WIN
#pragma comment(lib, "Bcrypt.lib")
#endif
TLS_DEFINE(uint64_t, _rand, 1)//_xorshift64 的状态，0 表示本线程还没播种

// xorshift64 伪随机数生成器(13/7/17 三次移位异或，不带末尾乘法)，线程局部状态，首次调用自动用状态槽地址+时间戳初始化种子
static uint64_t _xorshift64(void) {
    uint64_t *st = _rand_tls();
    if (0 == *st) {
        /* 首次调用：用本线程状态槽的地址(各线程不同)与时间戳组合初始化种子，避免种子为 0 */
        *st = (uint64_t)(uintptr_t)st ^ (nowms() * 6364136223846793005ULL + 1442695040888963407ULL);
        if (0 == *st){
            *st = 1;
        }
    }
    uint64_t x = *st;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *st = x;
    return x;
}
int32_t randrange(int32_t min, int32_t max) {
    // 闭区间 [min, max]：允许 max==min 退化情况返回 min；range==1 时 % 1 = 0 自然处理 
    ASSERTAB(max >= min, "rand range max must >= min.");
    uint32_t range = ((uint32_t)max - (uint32_t)min) + 1;
    if (0 == range) {
        return (int32_t)(_xorshift64() >> 32);
    }
    return (int32_t)((uint32_t)min + (uint32_t)(_xorshift64() % range));
}
// buf 必须至少分配 len+1 字节；函数在 buf[len] 处写 '\0'。
// 每个 64 位随机数按 6 位切出字符，落在 62 个字符以外的丢掉重取(不偏)，不必每个字符一次取模
char *randstr(char *buf, size_t len) {
    static char characters[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U',
        'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
        'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };
    size_t i = 0;
    uint64_t r = 0;
    int32_t bits = 0;
    uint32_t v;
    while (i < len) {
        if (bits < 6) {
            r = _xorshift64();
            bits = 64;
        }
        v = (uint32_t)(r & 63);
        r >>= 6;
        bits -= 6;
        if (v < sizeof(characters)) {
            buf[i++] = characters[v];
        }
    }
    buf[i] = '\0';
    return buf;
}
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_BSD)
// 反复取直到填满：一次调用未必给够，EINTR 之类可重试错误继续，其余即失败。
// fd < 0 走 getrandom(2)，否则从该 fd 读
static int32_t _rand_drain(void *buf, size_t len, int32_t fd) {
    size_t got = 0;
    ssize_t ret;
    while (got < len) {
#if CSPRNG_GETRANDOM
        if (fd < 0) {
            ret = syscall(SYS_getrandom, (char *)buf + got, len - got, 0);
        } else {
            ret = read(fd, (char *)buf + got, len - got);
        }
#else
        ret = read(fd, (char *)buf + got, len - got);
#endif
        if (ret < 0) {
            if (ERR_RW_RETRIABLE(ERRNO)) {
                continue;
            }
            return ERR_FAILED;
        }
        if (0 == ret) {
            return ERR_FAILED;
        }
        got += (size_t)ret;
    }
    return ERR_OK;
}
// 读 /dev/urandom，缓存 fd 省掉每次 open+close。
// 存的是 fd+1: 0 表示未初始化, 否则真 fd = 值-1 —— daemon 关掉 stdin 后 fd 会是 0,
// 不加偏移就分不清"没初始化"和"fd 就是 0"。fd 长期持有, 进程退出交给 OS 清理
static int32_t _rand_urandom(void *buf, size_t len) {
    static atomic_t _urand_fd_plus1 = 0;
    int32_t fd;
    atomic_t cur = ATOMIC_GET(&_urand_fd_plus1);
    if (0 == cur) {
        int32_t newfd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (newfd < 0) {
            return ERR_FAILED;
        }
        if (ATOMIC_CAS(&_urand_fd_plus1, 0, (atomic_t)(newfd + 1))) {
            fd = newfd;
        } else {
            // 并发首次 init：其他线程已 CAS 成功，关闭本线程的 fd 复用对方的
            close(newfd);
            fd = (int32_t)ATOMIC_GET(&_urand_fd_plus1) - 1;
        }
    } else {
        fd = (int32_t)cur - 1;
    }
    return _rand_drain(buf, len, fd);
}
#endif
int32_t csprng_rand(void *buf, size_t len) {
#if defined(OS_WIN)
    /* Windows：BCryptGenRandom 使用系统首选 CSPRNG，不依赖进程安全句柄。
     * 长度参数是 ULONG，超过的分段取，直接强转会截断、剩下的字节没填却报成功 */
    PUCHAR p = (PUCHAR)buf;
    ULONG n;
    while (len > 0) {
        n = len > ULONG_MAX ? ULONG_MAX : (ULONG)len;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, p, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            return ERR_FAILED;
        }
        p += n;
        len -= n;
    }
    return ERR_OK;
#elif defined(OS_DARWIN) || defined(OS_BSD)
    /* Darwin / BSD（macOS、FreeBSD、NetBSD、OpenBSD、DragonFly）：
     * arc4random_buf 由内核 CSPRNG 支撑，永不失败，无需检查返回值。*/
    arc4random_buf(buf, len);
    return ERR_OK;
#else
#if CSPRNG_GETRANDOM
    /* Linux：getrandom(2)（内核 3.17+），阻塞直至熵池就绪。老内核报 ENOSYS、seccomp 拦掉时报 EPERM
     * （此时一个字节都没填），记下后改读 /dev/urandom */
    static atomic_t _getrandom_nosys = 0;
    if (0 == ATOMIC_GET(&_getrandom_nosys)) {
        if (ERR_OK == _rand_drain(buf, len, -1)) {
            return ERR_OK;
        }
        if (ENOSYS != ERRNO
            && EPERM != ERRNO) {
            return ERR_FAILED;
        }
        ATOMIC_SET_RELAXED(&_getrandom_nosys, 1);
    }
#endif
    /* 其余 Unix（Solaris、AIX、HP-UX 等）与退回的 Linux 读 /dev/urandom */
    return _rand_urandom(buf, len);
#endif
}
