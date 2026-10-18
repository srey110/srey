#include "utils/popen2.h"
#include "utils/netutils.h"
#include "utils/idgen.h"
#if defined(OS_LINUX)
#include <sys/syscall.h>
#endif
#ifndef OS_WIN
#include <spawn.h>
extern char **environ;
#endif

// popen_waitexit 等子进程退出的办法：Linux 用 pidfd(内核 5.3+)，kqueue 平台用 EVFILT_PROC，
// 都没有、或运行时拿不到就退避轮询。等到事件后若还探不到退出(退出通知与可收尸之间的先后各内核不同)，
// 同样落到轮询把剩余时间等完，不当超时报
#if defined(OS_LINUX) && defined(SYS_pidfd_open)
    #define POPEN_WAIT_PIDFD
#elif defined(EV_KQUEUE)
    #define POPEN_WAIT_KQUEUE
#endif

#ifdef OS_WIN
#define PIPE_STDIN_SIZE  ONEK * 16
#define PIPE_STDOUT_SIZE ONEK * 64
#define PIPE_PREFIX      "\\\\.\\pipe\\LOCAL\\srey_pipe_"

// 建一条单向命名管道：tochild 为 1 时本进程写、子进程读（接 stdin），为 0 时反过来（接 stdout / stderr）。
// 本进程端 parent 不可继承、非阻塞；子进程端 child 可继承、不带重叠标志（子进程做同步读写，重叠句柄会误报完成）。
// 管道名整台机器共用，createid 只在进程内唯一，故名字带进程号；再带一段随机值并只许建第一个实例：
// 名字猜不到，同名管道已被别人建过就失败，不会连到别人的管道上。失败时两个出参都不动
static int32_t _popen_pipe(HANDLE *parent, HANDLE *child, int32_t tochild) {
    uint64_t salt;
    if (ERR_OK != csprng_rand(&salt, sizeof(salt))) {
        LOG_ERROR("csprng_rand failed.");
        return ERR_FAILED;
    }
    char pname[256];
    SNPRINTF(pname, sizeof(pname), "%s%lu_%"PRIu64"_%016"PRIx64,
             PIPE_PREFIX, (unsigned long)GetCurrentProcessId(), createid(), salt);
    // 本进程端设非阻塞，与 POSIX 侧那句 sock_nonblock 对应：子进程不读时 popen_write 不会把调用线程挂住
    HANDLE server = CreateNamedPipe(pname,//全局唯一的管道名称
                                    (tochild ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) | FILE_FLAG_FIRST_PIPE_INSTANCE,//单向，只建第一个实例
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,//字节流，非阻塞
                                    1,//该管道名称的最大实例数
                                    tochild ? PIPE_STDIN_SIZE : 0,//输出缓冲区大小（字节）
                                    tochild ? 0 : PIPE_STDOUT_SIZE,//输入缓冲区大小（字节）
                                    0,//超时为零，即使用默认 50 毫秒超时
                                    NULL);//不可继承
    if (INVALID_HANDLE_VALUE == server) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = NULL;//使用系统默认安全描述符
    sa.bInheritHandle = TRUE;//交给子进程继承
    HANDLE client = CreateFile(pname,
                               tochild ? (GENERIC_READ | FILE_WRITE_ATTRIBUTES) : (GENERIC_WRITE | FILE_READ_ATTRIBUTES),
                               0,
                               &sa,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               NULL);
    if (INVALID_HANDLE_VALUE == client) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        CloseHandle(server);
        return ERR_FAILED;
    }
    // 两端都已建好，连接早已成立：返回失败且错误码为 ERROR_PIPE_CONNECTED 是常态
    if (!ConnectNamedPipe(server, NULL)) {
        DWORD err = GetLastError();
        if (ERROR_PIPE_CONNECTED != err) {
            LOG_ERROR("%s", ERRORSTR(err));
            CloseHandle(client);
            CloseHandle(server);
            return ERR_FAILED;
        }
    }
    *parent = server;
    *child = client;
    return ERR_OK;
}
// 收集交给子进程继承的句柄：三个标准流里有效、可继承且不重复的。本进程没设成可继承的标准流照旧不带过去
static DWORD _popen_inherit_list(const STARTUPINFO *si, HANDLE list[3]) {
    HANDLE std[3] = { si->hStdInput, si->hStdOutput, si->hStdError };
    DWORD n = 0;
    DWORD flags, j;
    for (int32_t i = 0; i < 3; i++) {
        if (NULL == std[i]
            || INVALID_HANDLE_VALUE == std[i]
            || !GetHandleInformation(std[i], &flags)
            || 0 == (flags & HANDLE_FLAG_INHERIT)) {
            continue;
        }
        for (j = 0; j < n; j++) {
            if (list[j] == std[i]) {
                break;
            }
        }
        if (j == n) {
            list[n++] = std[i];
        }
    }
    return n;
}
// 建只放行 list 里这 n 个句柄的继承白名单；系统只记 list 的指针，它要活到属性表释放为止
static LPPROC_THREAD_ATTRIBUTE_LIST _popen_attrs_new(HANDLE *list, DWORD n) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &size);// 只取所需大小，这一次按约定返回失败
    LPPROC_THREAD_ATTRIBUTE_LIST attrs;
    MALLOC(attrs, size);
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        FREE(attrs);
        return NULL;
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, n * sizeof(HANDLE), NULL, NULL)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        DeleteProcThreadAttributeList(attrs);
        FREE(attrs);
        return NULL;
    }
    return attrs;
}
#else
// posix_spawn 起 "sh -c cmd"，r / w 时把标准流接到 fd。自成进程组(pgid == 子进程 pid)，popen_close 才能用
// kill(-pgid) 连 sh 派生的孙进程一起杀，只杀 sh 的话 "a | b" 会把 a/b 留成孤儿。
// 本进程忽略的 SIGPIPE 会被继承，得恢复默认；不接管 stdin 时接 /dev/null，后台进程组读终端会被挂起。
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
    err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF);
    if (0 == err) {
        err = posix_spawnattr_setpgroup(&attr, 0);
    }
    if (0 == err) {
        sigset_t sigdef;
        sigemptyset(&sigdef);
        sigaddset(&sigdef, SIGPIPE);
        err = posix_spawnattr_setsigdefault(&attr, &sigdef);
    }
    if (0 == err && r) {
        err = posix_spawn_file_actions_adddup2(&acts, fd, STDOUT_FILENO);
    }
    if (0 == err && r) {
        err = posix_spawn_file_actions_adddup2(&acts, fd, STDERR_FILENO);
    }
    // stdin 放最后：本进程 stdin 关着时 fd 可能正是 0，先接 /dev/null 会把它顶掉
    if (0 == err) {
        err = w ? posix_spawn_file_actions_adddup2(&acts, fd, STDIN_FILENO)
            : posix_spawn_file_actions_addopen(&acts, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
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
    // stdin 单独一条管道，popen_close_stdin 关掉它子进程就读到 EOF
    HANDLE child_in = NULL, child_out = NULL;
    if ((w && ERR_OK != _popen_pipe(&ctx->pipe[1], &child_in, 1))
        || (r && ERR_OK != _popen_pipe(&ctx->pipe[0], &child_out, 0))) {
        CLOSE_HANDLE(child_in);
        popen_free(ctx);
        return ERR_FAILED;
    }
    STARTUPINFOEX six;
    ZERO(&six, sizeof(six));
    six.StartupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    six.StartupInfo.wShowWindow = SW_HIDE;
    // 设了 STARTF_USESTDHANDLES 三个句柄就都按这里给的来：没重定向的那几个沿用本进程的；
    // 留 NULL 的话那一路不跟随本进程的重定向
    six.StartupInfo.hStdInput = w ? child_in : GetStdHandle(STD_INPUT_HANDLE);
    six.StartupInfo.hStdOutput = r ? child_out : GetStdHandle(STD_OUTPUT_HANDLE);
    six.StartupInfo.hStdError = r ? child_out : GetStdHandle(STD_ERROR_HANDLE);
    HANDLE inherit[3];
    DWORD ninherit = _popen_inherit_list(&six.StartupInfo, inherit);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    if (ninherit > 0) {
        attrs = _popen_attrs_new(inherit, ninherit);
        if (NULL == attrs) {
            CLOSE_HANDLE(child_in);
            CLOSE_HANDLE(child_out);
            popen_free(ctx);
            return ERR_FAILED;
        }
    }
    six.lpAttributeList = attrs;
    six.StartupInfo.cb = NULL == attrs ? sizeof(STARTUPINFO) : sizeof(STARTUPINFOEX);
    ctx->job = CreateJobObject(NULL, NULL);
    // 子进程只继承白名单里的句柄，本进程别的可继承句柄（如别的线程正在起的 popen 的管道）不带过去
    BOOL created = CreateProcess(NULL,
                                 (char *)cmd,
                                 NULL,
                                 NULL,
                                 NULL != attrs,
                                 CREATE_SUSPENDED | (NULL == attrs ? 0 : EXTENDED_STARTUPINFO_PRESENT),
                                 NULL,
                                 NULL,
                                 &six.StartupInfo,
                                 &ctx->process);
    if (!created) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
    if (NULL != attrs) {
        DeleteProcThreadAttributeList(attrs);
        FREE(attrs);
    }
    // 本进程这份子进程端得关掉，否则子进程退出后读端等不到 EOF
    CLOSE_HANDLE(child_in);
    CLOSE_HANDLE(child_out);
    if (!created) {
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
        CLOSE_SOCK(sock[0]);
        CLOSE_SOCK(sock[1]);
        return ERR_FAILED;
    }
    ctx->pid = pid;
    if (r || w) {
        close(sock[0]);
        ctx->sock = sock[1];
        ctx->wopen = w;
        ctx->ropen = r;
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
// 非阻塞探子进程是否已退出，不收尸（收尸只在 popen_close，之前进程组号不会被复用）：
// 已退出则记下退出码返回 1，还在跑返回 0，waitid 出错返回 ERR_FAILED。
// si 先清零、有没有结果看 si_signo；macOS 对停住的子进程也可能返回，故只认三种退出类 si_code
static int32_t _popen_probe(popen_ctx *ctx) {
    siginfo_t si;
    int32_t rtn;
    ZERO(&si, sizeof(si));
    while (-1 == (rtn = waitid(P_PID, (id_t)ctx->pid, &si, WEXITED | WNOHANG | WNOWAIT)) && EINTR == errno) {
    }
    if (ERR_FAILED == rtn) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    if (SIGCHLD != si.si_signo) {
        return 0;
    }
    if (CLD_EXITED == si.si_code) {
        ctx->exitcode = si.si_status;
    } else if (CLD_KILLED == si.si_code
        || CLD_DUMPED == si.si_code) {
        ctx->exitcode = ERR_FAILED;
    } else {
        return 0;
    }
    ctx->exited = 1;
    return 1;
}
#if defined(POPEN_WAIT_PIDFD)
// 阻塞到子进程退出或 tm 计时满 ms 毫秒，不收尸，由调用方再 _popen_probe。
// 已退出没收尸的子进程也拿得到 pidfd 并立刻可读。拿不到(老内核、容器 seccomp)或 poll 出错返回 ERR_FAILED，
// 调用方退回轮询
static int32_t _popen_wait_event(popen_ctx *ctx, timer_ctx *tm, uint32_t ms) {
    int32_t fd = (int32_t)syscall(SYS_pidfd_open, ctx->pid, 0);
    if (fd < 0) {
        return ERR_FAILED;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    uint64_t elapsed, left;
    int32_t r, rtn = ERR_OK;
    for (;;) {
        elapsed = timer_elapsed_ms(tm);
        if (elapsed >= ms) {
            break;
        }
        left = ms - elapsed;
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
// 同上，kqueue 版。注册时子进程已退出：macOS 返 ESRCH、FreeBSD 立即触发，两种都直接去探；
// 注册成功之后才退出的，NOTE_EXIT 一定会到
static int32_t _popen_wait_event(popen_ctx *ctx, timer_ctx *tm, uint32_t ms) {
    int32_t kq = kqueue();
    if (kq < 0) {
        return ERR_FAILED;
    }
    struct kevent ev;
    int32_t rtn = ERR_OK;
    EV_SET(&ev, ctx->pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0) {
        rtn = ESRCH == errno ? ERR_OK : ERR_FAILED;
        close(kq);
        return rtn;
    }
    struct timespec ts;
    uint64_t elapsed, left;
    int32_t r;
    for (;;) {
        elapsed = timer_elapsed_ms(tm);
        if (elapsed >= ms) {
            break;
        }
        left = ms - elapsed;
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
    if (ctx->closed) {
        return;
    }
    ctx->closed = 1;
#ifdef OS_WIN
    if (NULL == ctx->process.hProcess
        || 0 == ctx->process.dwProcessId) {
        return;
    }
    // 子进程退没退都结束整个作业：它派生的孙进程可能还在跑。作业里已没有进程时这一步什么也不做
    if (NULL != ctx->job
        && TerminateJobObject(ctx->job, ERR_FAILED)) {
        return;
    }
    TerminateProcess(ctx->process.hProcess, ERR_FAILED);//已退出的进程上调用只是失败，退出码不变
#else
    if (0 == ctx->pid) {
        return;
    }
    // 子进程退没退都杀整个进程组：posix_spawn 建的子进程 pgid == ctx->pid，sh 派生的孙进程都在组里；
    // 它还没收尸，组号不会被别的进程复用。组里已没有进程时 kill(-pid) 返 ESRCH，再退化成只打 sh 自己
    if (0 != kill(-ctx->pid, SIGKILL)) {
        kill(ctx->pid, SIGKILL);
    }
    // 收尸只在这里做。退出码按 wstatus 真值解析：子进程可能早已自己正常退完，无条件写 ERR_FAILED 会把 exit(0) 报成 -1
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
    CLOSE_SOCK(ctx->sock);
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
    int32_t r = _popen_probe(ctx);
    if (0 != r) {
        return 1 == r ? ERR_OK : ERR_FAILED;
    }
    if (0 == ms) {
        return ERR_FAILED;
    }
    // 单调计时：墙钟被回拨或前跳时超时会提前到期或拖长
    timer_ctx tm;
    timer_init(&tm);
#if defined(POPEN_WAIT_PIDFD) || defined(POPEN_WAIT_KQUEUE)
    if (ERR_OK == _popen_wait_event(ctx, &tm, ms)) {
        r = _popen_probe(ctx);
        if (0 != r) {
            return 1 == r ? ERR_OK : ERR_FAILED;
        }
    }
#endif
    uint32_t sleep_ms = 1;
    uint64_t elapsed;
    uint32_t remaining, s;
    for (;;) {
        r = _popen_probe(ctx);
        if (0 != r) {
            return 1 == r ? ERR_OK : ERR_FAILED;
        }
        elapsed = timer_elapsed_ms(&tm);
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
    if (NULL == ctx->process.hProcess
        || WAIT_OBJECT_0 != WaitForSingleObject(ctx->process.hProcess, 0)) {
        return ERR_FAILED;// 还没退出：不把 STILL_ACTIVE(259) 当退出码交出去
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
    if (ctx->exited
        || 1 == _popen_probe(ctx)) {
        return ctx->exitcode;
    }
    return ERR_FAILED;
#endif
}
int32_t popen_read(popen_ctx *ctx, char *output, size_t lens, int32_t *eof) {
    SET_PTR(eof, 0);
#ifdef OS_WIN
    if (NULL == ctx->pipe[0]) {
        return ERR_FAILED;
    }
    DWORD nread;
    if (!PeekNamedPipe(ctx->pipe[0], NULL, 0, NULL, &nread, NULL)) {
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
    if (!ReadFile(ctx->pipe[0], output, (DWORD)lens, &nread, NULL)) {
        return ERR_FAILED;
    }
    return (int32_t)nread;
#else
    if (INVALID_SOCK == ctx->sock
        || !ctx->ropen) {
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
    if (lens > INT32_MAX) {
        lens = INT32_MAX;// 返回值是 int32_t，单次最多写这么多，余下按部分写交还调用方
    }
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
    if (INVALID_SOCK == ctx->sock
        || !ctx->wopen) {
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
int32_t popen_close_stdin(popen_ctx *ctx) {
#ifdef OS_WIN
    if (NULL == ctx->pipe[1]) {
        return ERR_FAILED;
    }
    CLOSE_HANDLE(ctx->pipe[1]);// stdin 是单独一条管道，关掉写端子进程就读到 EOF
    return ERR_OK;
#else
    if (INVALID_SOCK == ctx->sock
        || !ctx->wopen) {
        return ERR_FAILED;
    }
    ctx->wopen = 0;
    // 只关写方向：子进程读 stdin 见 EOF，它的输出照常可读。子进程已关掉它那端时返 ENOTCONN，不算失败
    if (0 != shutdown(ctx->sock, SHUT_WR)
        && ENOTCONN != errno) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
#endif
}
