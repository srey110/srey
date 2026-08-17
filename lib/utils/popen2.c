#include "utils/popen2.h"
#include "utils/netutils.h"
#include "utils/utils.h"

#ifdef OS_WIN
#define PIPE_INBUF_SIZE  ONEK * 16
#define PIPE_OUTBUF_SIZE ONEK * 64
#define PIPE_PREFIX      "\\\\.\\pipe\\LOCAL\\srey_pipe_"

// Windows 下创建命名管道对，供子进程与父进程通信
static int32_t _popen_pipe(HANDLE pipe[2]) {
    char pname[256];
    SNPRINTF(pname, sizeof(pname), "%s%"PRIu64, PIPE_PREFIX, createid());
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
    if (w) {
        startup.hStdInput = ctx->pipe[0];//子进程标准输入重定向到管道
    }
    if (r) {
        startup.hStdError = ctx->pipe[0];//子进程标准错误重定向到管道
        startup.hStdOutput = ctx->pipe[0];//子进程标准输出重定向到管道
    }
    if (!CreateProcess(NULL,
                      TEXT((char *)cmd),
                      NULL,
                      NULL,
                      TRUE,
                      0,
                      NULL,
                      NULL,
                      &startup,
                      &ctx->process)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        popen_free(ctx);
        return ERR_FAILED;
    }
    if (NULL != ctx->pipe[0]) {
        CloseHandle(ctx->pipe[0]);
        ctx->pipe[0] = NULL;
    }
#else
    SOCKET sock[2];
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
    pid_t pid = fork();
    if (0 == pid) {
        //自成进程组(pgid == 本进程 pid),popen_close 才能用 kill(-pgid) 连 sh 派生的孙进程一起杀。
        //只杀 sh 的话 "a | b" 这种复合命令会把 a/b 留成孤儿。失败不致命,退化成只杀直接子进程
        (void)!setpgid(0, 0);
        if (w) {
            dup2(sock[0], STDIN_FILENO);
        }
        if (r) {
            dup2(sock[0], STDOUT_FILENO);
            dup2(sock[0], STDERR_FILENO);
        }
        if (r || w) {
            close(sock[0]);
            close(sock[1]);
        }
        execl("/bin/sh", "sh", "-c", cmd, NULL);
        //fork 后子进程严格只能调 async-signal-safe 函数；log 走 fsqu+malloc+cond 不安全，
        //且子进程未继承日志消费线程，入队消息无人消费；exit() 会 fflush 父子共享的 stdio buffer。
        //改用 write + _exit（均 async-signal-safe），约定退出码 127 表示 exec 失败（shell 惯例）。
        const char prefix[] = "popen execl failed: ";
        (void)!write(STDERR_FILENO, prefix, sizeof(prefix) - 1);
        (void)!write(STDERR_FILENO, cmd, strlen(cmd));
        (void)!write(STDERR_FILENO, "\n", 1);
        _exit(127);
    } else if (pid > 0) {
        ctx->pid = pid;
        if (r || w) {
            close(sock[0]);
            ctx->sock = sock[1];
            sock_nonblock(ctx->sock);
        }
        return ERR_OK;
    } else {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        if (r || w) {
            close(sock[0]);
            close(sock[1]);
        }
        return ERR_FAILED;
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
#ifdef WCOREDUMP
    if (WCOREDUMP(wstatus)) {//core dump
        ctx->exited = 1;
        ctx->exitcode = ERR_FAILED;
        return ERR_OK;
    }
#endif
    return ERR_FAILED;
}
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
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);//获取当前系统进程快照
    if (NULL == snapshot) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        TerminateProcess(ctx->process.hProcess, ERR_FAILED);
        return;
    }
    HANDLE pvchild;
    PROCESSENTRY32 proentry32;
    proentry32.dwSize = sizeof(PROCESSENTRY32);
    BOOL ok = Process32First(snapshot, &proentry32);//枚举第一个进程
    while (ok) {
        if (proentry32.th32ParentProcessID == ctx->process.dwProcessId) {
            pvchild = OpenProcess(PROCESS_ALL_ACCESS, FALSE, proentry32.th32ProcessID);
            if (NULL != pvchild) {
                TerminateProcess(pvchild, ERR_FAILED);
                CloseHandle(pvchild);
            } else {
                LOG_ERROR("%s", ERRORSTR(ERRNO));
            }
        }
        ok = Process32Next(snapshot, &proentry32);
    }
    TerminateProcess(ctx->process.hProcess, ERR_FAILED);
    CloseHandle(snapshot);
#else
    if (0 != ctx->pid && !ctx->exited) {
        // 杀整个进程组:子进程 setpgid(0,0) 后 pgid == ctx->pid,sh 派生的孙进程都在组里。
        // 组不存在(setpgid 失败)时 kill(-pid) 返 ESRCH,再退化成只打 sh 自己
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
    if (NULL != ctx->process.hProcess) {
        CloseHandle(ctx->process.hProcess);
        ctx->process.hProcess = NULL;
    }
    if (NULL != ctx->process.hThread) {
        CloseHandle(ctx->process.hThread);
        ctx->process.hThread = NULL;
    }
    if (NULL != ctx->pipe[0]) {
        CloseHandle(ctx->pipe[0]);
        ctx->pipe[0] = NULL;
    }
    if (NULL != ctx->pipe[1]) {
        CloseHandle(ctx->pipe[1]);
        ctx->pipe[1] = NULL;
    }
#else
    if (INVALID_SOCK != ctx->sock) {
        shutdown(ctx->sock, SHUT_RD);
        close(ctx->sock);
        ctx->sock = INVALID_SOCK;
    }
#endif
}
#ifndef OS_WIN
// 非阻塞探测 sock 是否可读：1=就绪可读，0=未就绪，ERR_FAILED=poll 出错（EINTR 已重试）
static int32_t _popen_poll_readable(int32_t sock) {
    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    int32_t r;
    do {
        r = poll(&pfd, 1, 0);
    } while (r < 0 && EINTR == errno);
    if (r < 0) {
        return ERR_FAILED;
    }
    return (0 == r) ? 0 : 1;
}
// 非阻塞检查套接字是否已关闭（对端断开），返回 1 表示已关闭
static int32_t _popen_sock_closed(int32_t sock) {
    int32_t r = _popen_poll_readable(sock);
    if (0 == r) {
        return 0;
    }
    if (ERR_FAILED == r) {
        return 1;
    }
    return sock_nread(sock) <= 0;
}
#endif
int32_t popen_waitexit(popen_ctx *ctx, uint32_t ms) {
#ifdef OS_WIN
    if (NULL == ctx->process.hProcess) {
        return ERR_OK;
    }
    if (WAIT_TIMEOUT == WaitForSingleObject(ctx->process.hProcess, (DWORD)ms)) {
        return ERR_FAILED;
    }
    return ERR_OK;
#else
    if (0 == ctx->pid || ctx->exited) {
        return ERR_OK;
    }
    if (INVALID_SOCK != ctx->sock
        && _popen_sock_closed(ctx->sock)) {
        int wstatus;
        pid_t rtn = waitpid(ctx->pid, &wstatus, WNOHANG);
        if (ctx->pid == rtn) {
            _popen_child_exited(ctx, wstatus);
            return ERR_OK;
        }
    }
    pid_t rtn;
    int wstatus;
    uint64_t startms = nowms();
    uint32_t sleep_ms = 1;
    uint64_t elapsed;
    uint32_t remaining, s;
    for (;;) {
        while (-1 == (rtn = waitpid(ctx->pid, &wstatus, WNOHANG)) && EINTR == errno) {
        }
        if (ERR_FAILED == rtn) {
            LOG_ERROR("%s", ERRORSTR(ERRNO));
            return ERR_FAILED;
        }
        if (ctx->pid == rtn) {
            if (ERR_OK == _popen_child_exited(ctx, wstatus)) {
                return ERR_OK;
            }
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
    int32_t r = _popen_poll_readable(ctx->sock);
    if (ERR_FAILED == r) {
        return ERR_FAILED;
    }
    if (0 == r) {
        return 0;
    }
    ssize_t rn;
    do {
        rn = read(ctx->sock, output, lens);
    } while (-1 == rn && EINTR == errno);
    if (-1 == rn) {
        return ERR_FAILED;
    }
    if (0 == rn) {
        // poll 就绪却读到 0 字节:写端全关,即 EOF
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
