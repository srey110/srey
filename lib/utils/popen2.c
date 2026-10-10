#include "utils/popen2.h"
#include "utils/netutils.h"
#include "utils/utils.h"
#if defined(OS_LINUX)
#include <sys/syscall.h>
#endif
#ifndef OS_WIN
#include <spawn.h>
extern char **environ;
#endif

// popen_waitexit 等子进程退出的办法：Linux 用 pidfd(内核 5.3+)，kqueue 平台用 EVFILT_PROC，
// 都没有、或运行时拿不到就退避轮询。等到事件后若还收不了尸(退出通知与可收尸之间的先后各内核不同)，
// 同样落到轮询把剩余时间等完，不当超时报
#if defined(OS_LINUX) && defined(SYS_pidfd_open)
    #define POPEN_WAIT_PIDFD
#elif defined(EV_KQUEUE)
    #define POPEN_WAIT_KQUEUE
#endif

#ifdef OS_WIN
#define PIPE_INBUF_SIZE  ONEK * 16
#define PIPE_OUTBUF_SIZE ONEK * 64
#define PIPE_PREFIX      "\\\\.\\pipe\\LOCAL\\srey_pipe_"

// Windows 下创建命名管道对，供子进程与父进程通信。管道名整台机器共用，createid 只在进程内唯一，故名字带进程号
static int32_t _popen_pipe(HANDLE pipe[2]) {
    char pname[256];
    SNPRINTF(pname, sizeof(pname), "%s%lu_%"PRIu64, PIPE_PREFIX, (unsigned long)GetCurrentProcessId(), createid());
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;//使用系统默认安全描述符
    sa.bInheritHandle = TRUE;//允许子进程继承句柄
    HANDLE server = CreateNamedPipe(pname,//全局唯一的管道名称
                                    PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,//双向异步打开模式
                                    PIPE_TYPE_BYTE,//字节流管道模式
                                    1,//该管道名称的最大实例数
                                    PIPE_OUTBUF_SIZE,//输出缓冲区大小（字节）
                                    PIPE_INBUF_SIZE,//输入缓冲区大小（字节）
                                    0,//超时为零，即使用默认 50 毫秒超时
                                    &sa);
    if (INVALID_HANDLE_VALUE == server) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    HANDLE event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (NULL == event) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CloseHandle(server);
        return ERR_FAILED;
    }
    OVERLAPPED ovlpd = { 0 };
    ovlpd.hEvent = event;
    if (!ConnectNamedPipe(server, &ovlpd)) {
        int32_t err = GetLastError();
        if (ERROR_PIPE_CONNECTED == err) {
            SetEvent(event);
        } else if (ERROR_IO_PENDING != err) {
            LOG_ERROR("%s", ERRORSTR(err));
            CloseHandle(event);
            CloseHandle(server);
            return ERR_FAILED;
        }
    }
    HANDLE client = CreateFile(pname,
                               GENERIC_READ | GENERIC_WRITE,
                               0,
                               NULL,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               NULL);
    if (INVALID_HANDLE_VALUE == client) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CloseHandle(event);
        CloseHandle(server);
        return ERR_FAILED;
    }
    if (WAIT_FAILED == WaitForSingleObject(event, INFINITE)) {
        CloseHandle(event);
        CloseHandle(server);
        CloseHandle(client);
        return ERR_FAILED;
    }
    CloseHandle(event);
    // 父端设非阻塞:与 POSIX 侧那句 sock_nonblock 对应。不设的话子进程不读时
    // popen_write 的 WriteFile 会把派发线程一直挂住,正是 POSIX 侧修掉的那个死法。
    // 读侧本来就先 PeekNamedPipe 再读,不受影响
    DWORD nowait = PIPE_NOWAIT;
    SetNamedPipeHandleState(client, &nowait, NULL, NULL);
    pipe[0] = server;
    pipe[1] = client;
    return ERR_OK;
}
#else
// posix_spawn 起 "sh -c cmd"，r / w 时把标准流接到 fd。自成进程组(pgid == 子进程 pid)，popen_close 才能用
// kill(-pgid) 连 sh 派生的孙进程一起杀，只杀 sh 的话 "a | b" 会把 a/b 留成孤儿。
// 每一步都查返回值，哪步失败都不起子进程，免得带着没设好的重定向跑起来；返回 0 或错误码
static int32_t _popen_spawn(pid_t *pid, const char *cmd, int32_t r, int32_t w, SOCKET fd) {
    posix_spawn_file_actions_t acts;
    posix_spawnattr_t attr;
    char *argv[] = { "sh", "-c", (char *)cmd, NULL };
    int32_t err = posix_spawn_file_actions_init(&acts);
    if (0 != err) {
        return err;
    }
    err = posix_spawnattr_init(&attr);
    if (0 != err) {
        posix_spawn_file_actions_destroy(&acts);
        return err;
    }
    err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    if (0 == err) {
        err = posix_spawnattr_setpgroup(&attr, 0);
    }
    if (0 == err && w) {
        err = posix_spawn_file_actions_adddup2(&acts, fd, STDIN_FILENO);
    }
    if (0 == err && r) {
        err = posix_spawn_file_actions_adddup2(&acts, fd, STDOUT_FILENO);
    }
    if (0 == err && r) {
        err = posix_spawn_file_actions_adddup2(&acts, fd, STDERR_FILENO);
    }
    if (0 == err) {
        err = posix_spawn(pid, "/bin/sh", &acts, &attr, argv, environ);
    }
    posix_spawn_file_actions_destroy(&acts);
    posix_spawnattr_destroy(&attr);
    return err;
}
#endif
int32_t popen_startup(popen_ctx *ctx, const char *cmd, const char *mode) {
    // 必须在所有失败路径之前 ZERO,失败时调用方走 popen_free/popen_close 兜底能读到 NULL/INVALID 终止
    ZERO(ctx, sizeof(popen_ctx));
#ifndef OS_WIN
    ctx->sock = INVALID_SOCK;// 防 EMPTYSTR 早返，popen_free 关掉stdin(0)
#endif
    if (EMPTYSTR(cmd)) {
        return ERR_FAILED;
    }
    int32_t r = 0, w = 0;
    if (NULL != mode) {
        r = NULL != strchr(mode, 'r');
        w = NULL != strchr(mode, 'w');
    }
#ifdef OS_WIN
    if (r || w) {
        if (ERR_OK != _popen_pipe(ctx->pipe)) {
            return ERR_FAILED;
        }
    }
    STARTUPINFO startup = { 0 };
    startup.cb = sizeof(STARTUPINFO);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    // 设了 STARTF_USESTDHANDLES 三个句柄就都按这里给的来：没重定向的那几个沿用本进程的，同 POSIX 侧继承；
    // 留 NULL 的话那一路不跟随本进程的重定向
    startup.hStdInput = w ? ctx->pipe[0] : GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = r ? ctx->pipe[0] : GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = r ? ctx->pipe[0] : GetStdHandle(STD_ERROR_HANDLE);
    ctx->job = CreateJobObject(NULL, NULL);
    if (!CreateProcess(NULL,
                      TEXT((char *)cmd),
                      NULL,
                      NULL,
                      TRUE,
                      CREATE_SUSPENDED,
                      NULL,
                      NULL,
                      &startup,
                      &ctx->process)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        popen_free(ctx);
        return ERR_FAILED;
    }
    if (NULL != ctx->job
        && !AssignProcessToJobObject(ctx->job, ctx->process.hProcess)) {
        LOG_WARN("%s", ERRORSTR(ERRNO));
        CLOSE_HANDLE(ctx->job);
    }
    if ((DWORD)-1 == ResumeThread(ctx->process.hThread)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        TerminateProcess(ctx->process.hProcess, ERR_FAILED);
        popen_free(ctx);
        return ERR_FAILED;
    }
    CLOSE_HANDLE(ctx->pipe[0]);
#else
    SOCKET sock[2] = { INVALID_SOCK, INVALID_SOCK };
    if (r || w) {
        // AF_UNIX socketpair：进程私有、不耗端口，close 带未读数据是干净 EOF（TCP 环回会发 RST 破坏 popen_read 的 eof 语义）。
        // 建好后由下面那句 sock_nonblock 转为非阻塞——阻塞的话子进程不读就会把派发线程挂住
#if defined(SOCK_CLOEXEC)
        int32_t sprc = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sock);
#else
        int32_t sprc = socketpair(AF_UNIX, SOCK_STREAM, 0, sock);
        if (ERR_FAILED != sprc) {
            SET_CLOEXEC(sock[0]);
            SET_CLOEXEC(sock[1]);
        }
#endif
        if (ERR_FAILED == sprc) {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
            return ERR_FAILED;
        }
    }
    pid_t pid;
    int32_t err = _popen_spawn(&pid, cmd, r, w, sock[0]);
    if (0 != err) {
        LOG_ERROR("%s", ERRORSTR(err));
        if (r || w) {
            close(sock[0]);
            close(sock[1]);
        }
        return ERR_FAILED;
    }
    ctx->pid = pid;
    if (r || w) {
        close(sock[0]);
        ctx->sock = sock[1];
        sock_nonblock(ctx->sock);
    }
#endif
    return ERR_OK;
}
#ifndef OS_WIN
// 解析 waitpid 返回的 wstatus，判断子进程是否已退出并记录退出码
static int32_t _popen_child_exited(popen_ctx *ctx, int wstatus) {
    if (WIFEXITED(wstatus)) {//正常结束
        ctx->exited = 1;
        ctx->exitcode = WEXITSTATUS(wstatus);
        return ERR_OK;
    }
    if (WIFSIGNALED(wstatus)) {//信号而终止
        ctx->exited = 1;
        ctx->exitcode = ERR_FAILED;
        return ERR_OK;
    }
    // 只剩 stopped / continued 两种状态,而所有 waitpid 都不传 WUNTRACED / WCONTINUED,
    // 到不了这里;WCOREDUMP 也只在 WIFSIGNALED 为真时才有定义,不能在这一档求值
    return ERR_FAILED;
}
// 非阻塞收尸：已退出则记下退出码返回 1，还在跑返回 0，waitpid 出错返回 ERR_FAILED
static int32_t _popen_reap(popen_ctx *ctx) {
    int wstatus;
    pid_t rtn;
    while (-1 == (rtn = waitpid(ctx->pid, &wstatus, WNOHANG)) && EINTR == errno) {
    }
    if (ERR_FAILED == rtn) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    if (ctx->pid == rtn
        && ERR_OK == _popen_child_exited(ctx, wstatus)) {
        return 1;
    }
    return 0;
}
#if defined(POPEN_WAIT_PIDFD)
// 阻塞到子进程退出或到 deadline(nowms 毫秒)，不收尸，由调用方再 _popen_reap。
// 已退出没收尸的子进程也拿得到 pidfd 并立刻可读。拿不到(老内核、容器 seccomp)或 poll 出错返回 ERR_FAILED，
// 调用方退回轮询
static int32_t _popen_wait_event(popen_ctx *ctx, uint64_t deadline) {
    struct pollfd pfd;
    uint64_t now, left;
    int32_t r, rtn = ERR_OK;
    int32_t fd = (int32_t)syscall(SYS_pidfd_open, ctx->pid, 0);
    if (fd < 0) {
        return ERR_FAILED;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (;;) {
        now = nowms();
        if (now >= deadline) {
            break;
        }
        left = deadline - now;
        r = poll(&pfd, 1, left > INT32_MAX ? INT32_MAX : (int)left);
        if (r > 0) {
            break;
        }
        if (r < 0
            && EINTR != errno) {
            rtn = ERR_FAILED;
            break;
        }
    }
    close(fd);
    return rtn;
}
#elif defined(POPEN_WAIT_KQUEUE)
// 同上，kqueue 版。注册时子进程已退出：macOS 返 ESRCH、FreeBSD 立即触发，两种都直接去收尸；
// 注册成功之后才退出的，NOTE_EXIT 一定会到
static int32_t _popen_wait_event(popen_ctx *ctx, uint64_t deadline) {
    struct kevent ev;
    struct timespec ts;
    uint64_t now, left;
    int32_t r, rtn = ERR_OK;
    int32_t kq = kqueue();
    if (kq < 0) {
        return ERR_FAILED;
    }
    EV_SET(&ev, ctx->pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        rtn = ESRCH == errno ? ERR_OK : ERR_FAILED;
        close(kq);
        return rtn;
    }
    for (;;) {
        now = nowms();
        if (now >= deadline) {
            break;
        }
        left = deadline - now;
        ts.tv_sec = (time_t)(left / 1000);
        ts.tv_nsec = (long)(left % 1000) * 1000000L;
        r = kevent(kq, NULL, 0, &ev, 1, &ts);
        if (r > 0) {
            break;
        }
        if (r < 0
            && EINTR != errno) {
            rtn = ERR_FAILED;
            break;
        }
    }
    close(kq);
    return rtn;
}
#endif
#endif
void popen_close(popen_ctx *ctx) {
    ctx->closed = 1;
#ifdef OS_WIN
    if (NULL == ctx->process.hProcess
        || 0 == ctx->process.dwProcessId) {
        return;
    }
    DWORD exitcode;
    if (!GetExitCodeProcess(ctx->process.hProcess, &exitcode)) {//获取进程退出码
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return;
    }
    if (STILL_ACTIVE != exitcode) {//进程已经退出则直接返回
        return;
    }
    if (NULL != ctx->job
        && TerminateJobObject(ctx->job, ERR_FAILED)) {
        return;
    }
    TerminateProcess(ctx->process.hProcess, ERR_FAILED);
#else
    if (0 != ctx->pid && !ctx->exited) {
        // 杀整个进程组:posix_spawn 建的子进程 pgid == ctx->pid,sh 派生的孙进程都在组里。
        // 组不存在时 kill(-pid) 返 ESRCH,再退化成只打 sh 自己
        if (0 != kill(-ctx->pid, SIGKILL)) {
            kill(ctx->pid, SIGKILL);
        }
        // SIGKILL 后必须 waitpid 收尸，否则进程残留为 <defunct> 直至父进程退出。
        // 退出码按 wstatus 真值解析：子进程可能早已自己正常退完，无条件写 ERR_FAILED 会把 exit(0) 报成 -1
        int wstatus = 0;
        pid_t reaped;
        do {
            reaped = waitpid(ctx->pid, &wstatus, 0);
        } while (-1 == reaped && EINTR == errno);
        if (reaped != ctx->pid
            || ERR_OK != _popen_child_exited(ctx, wstatus)) {
            ctx->exited = 1;
            ctx->exitcode = ERR_FAILED;
        }
    }
#endif
}
void popen_free(popen_ctx *ctx) {
    if (0 == ctx->closed) {
        popen_close(ctx);
    }
#ifdef OS_WIN
    CLOSE_HANDLE(ctx->process.hProcess);
    CLOSE_HANDLE(ctx->process.hThread);
    CLOSE_HANDLE(ctx->job);
    CLOSE_HANDLE(ctx->pipe[0]);
    CLOSE_HANDLE(ctx->pipe[1]);
#else
    if (INVALID_SOCK != ctx->sock) {
        shutdown(ctx->sock, SHUT_RD);
        close(ctx->sock);
        ctx->sock = INVALID_SOCK;
    }
#endif
}
int32_t popen_waitexit(popen_ctx *ctx, uint32_t ms) {
#ifdef OS_WIN
    if (NULL == ctx->process.hProcess) {
        return ERR_OK;
    }
    // 超时与 WAIT_FAILED 都不算退出
    if (WAIT_OBJECT_0 != WaitForSingleObject(ctx->process.hProcess, (DWORD)ms)) {
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    if (0 == ctx->pid || ctx->exited) {
        return ERR_OK;
    }
    int32_t r = _popen_reap(ctx);
    if (0 != r) {
        return 1 == r ? ERR_OK : ERR_FAILED;
    }
    if (0 == ms) {
        return ERR_FAILED;
    }
    uint64_t startms = nowms();
#if defined(POPEN_WAIT_PIDFD) || defined(POPEN_WAIT_KQUEUE)
    if (ERR_OK == _popen_wait_event(ctx, startms + ms)) {
        r = _popen_reap(ctx);
        if (0 != r) {
            return 1 == r ? ERR_OK : ERR_FAILED;
        }
    }
#endif
    uint32_t sleep_ms = 1;
    uint64_t elapsed;
    uint32_t remaining, s;
    for (;;) {
        r = _popen_reap(ctx);
        if (0 != r) {
            return 1 == r ? ERR_OK : ERR_FAILED;
        }
        elapsed = nowms() - startms;
        if (elapsed >= ms) {
            return ERR_FAILED;
        }
        // 指数退避：1, 2, 4, 8, 16, 32ms 封顶；最后一次 sleep 不超剩余时间。
        remaining = (uint32_t)(ms - elapsed);
        s = sleep_ms < remaining ? sleep_ms : remaining;
        MSLEEP(s);
        if (sleep_ms < 32) {
            sleep_ms *= 2;
        }
    }
#endif
}
int32_t popen_exitcode(popen_ctx *ctx) {
#ifdef OS_WIN
    if (NULL == ctx->process.hProcess) {
        return ERR_FAILED;
    }
    DWORD dwcode;
    if (!GetExitCodeProcess(ctx->process.hProcess, &dwcode)) {//获得退出码
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return (int32_t)dwcode;
#else
    if (0 == ctx->pid) {
        return ERR_FAILED;
    }
    if (ctx->exited) {
        return ctx->exitcode;
    }
    int wstatus = 0;
    pid_t rtn;
    while (-1 == (rtn = waitpid(ctx->pid, &wstatus, WNOHANG)) && EINTR == errno) {
    }
    if (ctx->pid == rtn) {
        if (ERR_OK == _popen_child_exited(ctx, wstatus)) {
            return ctx->exitcode;
        }
    }
    return ERR_FAILED;
#endif
}
int32_t popen_read(popen_ctx *ctx, char *output, size_t lens, int32_t *eof) {
    SET_PTR(eof, 0);
#ifdef OS_WIN
    if (NULL == ctx->pipe[1]) {
        return ERR_FAILED;
    }
    DWORD nread;
    if (!PeekNamedPipe(ctx->pipe[1], NULL, 0, NULL, &nread, NULL)) {
        // 写端全部关闭时 Peek 报 broken pipe，据此判 EOF；其余才是真失败
        DWORD err = GetLastError();
        if (ERROR_BROKEN_PIPE == err || ERROR_PIPE_NOT_CONNECTED == err) {
            SET_PTR(eof, 1);
            return 0;
        }
        return ERR_FAILED;
    }
    if (0 == nread) {
        return 0;
    }
    if (!ReadFile(ctx->pipe[1], output, (DWORD)lens, &nread, NULL)) {
        return ERR_FAILED;
    }
    return (int32_t)nread;
#else
    if (INVALID_SOCK == ctx->sock) {
        return ERR_FAILED;
    }
    ssize_t rn;
    do {
        rn = read(ctx->sock, output, lens);
    } while (-1 == rn && EINTR == errno);
    if (-1 == rn) {
        return ERR_RW_RETRIABLE(errno) ? 0 : ERR_FAILED;
    }
    if (0 == rn) {
        // 读到 0 字节:写端全关,即 EOF
        SET_PTR(eof, 1);
        return 0;
    }
    return (int32_t)rn;
#endif
}
int32_t popen_write(popen_ctx *ctx, const char *input, size_t lens) {
#ifdef OS_WIN
    if (NULL == ctx->pipe[1]) {
        return ERR_FAILED;
    }
    // 与 POSIX 侧同一套语义:管道已设 PIPE_NOWAIT,写满即返回已写字节数(可能少于 lens),
    // 由调用方决定重试还是放弃
    DWORD nwrite;
    DWORD total = 0;
    DWORD remain = (DWORD)lens;
    while (total < remain) {
        if (!WriteFile(ctx->pipe[1], input + total, remain - total, &nwrite, NULL)) {
            return 0 == total ? ERR_FAILED : (int32_t)total;
        }
        if (0 == nwrite) {
            break;// PIPE_NOWAIT 下缓冲已满,本次写到这
        }
        total += nwrite;
    }
    return (int32_t)total;
#else
    if (INVALID_SOCK == ctx->sock) {
        return ERR_FAILED;
    }
    // 非阻塞 + 补写循环：一次 write 只写进对端缓冲剩下的那点空间是常态，
    // 写满即返回已写字节数（可能少于 lens），由调用方决定是重试还是放弃
    ssize_t nwrite;
    size_t total = 0;
    while (total < lens) {
        nwrite = write(ctx->sock, input + total, lens - total);
        if (nwrite > 0) {
            total += (size_t)nwrite;
            continue;
        }
        if (-1 == nwrite && EINTR == errno) {
            continue;
        }
        if (-1 == nwrite && ERR_RW_RETRIABLE(errno)) {
            break;// 对端缓冲已满，本次写到这
        }
        return 0 == total ? ERR_FAILED : (int32_t)total;
    }
    return (int32_t)total;
#endif
}
