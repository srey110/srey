#include "base/sys.h"
#include "base/err.h"
#include "base/macro_util.h"
#include "base/macro_atomic.h"
#include "base/macro_log.h"
#include "base/sfile.h"
#include "base/stime.h"
#include "base/once.h"
#if defined(OS_FBSD)
    #include <sys/param.h>
    #include <sys/cpuset.h>
#endif
#ifdef OS_WIN
// MiniDump 提权用的 OpenProcessToken / AdjustTokenPrivileges 等出自这里。
// WITH_SSL 时它由 OpenSSL 那组 pragma 顺带链上,关掉就缺,故本文件自己声明
#pragma comment(lib, "advapi32.lib")
static atomic_t _exindex = 0;
#endif

#if defined(OS_WIN)
// _MiniDump 传给 _dump_thread 的参数。da_err 必须由 dump 线程自己填:GetLastError 是
// 线程局部的,回到崩溃线程再读拿到的是别的值
typedef struct dump_arg {
    BOOL da_ok;
    DWORD da_err;
    DWORD da_tid;//崩溃线程 id, 决定 dump 打开后停在哪个线程
    HANDLE da_file;
    struct _EXCEPTION_POINTERS *da_excep;
}dump_arg;
#endif
static void *_ud;//信号处理回调的用户数据
static void(*_sig_cb)(int32_t, void *);//用户注册的信号处理回调函数
static char _path[PATH_LENS] = { 0 };//程序所在目录路径缓存
static once_ctx _path_once = 0;//路径只取一次，见 once.h

#ifdef OS_WIN
// 获取当前线程或进程的令牌句柄，用于后续权限操作
static BOOL _GetImpersonationToken(HANDLE *handle) {
    if (!OpenThreadToken(GetCurrentThread(),
                         TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
                         TRUE,
                         handle)) {
        if (ERROR_NO_TOKEN == ERRNO) {
            if (!OpenProcessToken(GetCurrentProcess(),
                                  TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
                                  handle)) {
                return FALSE;
            }
        } else {
            return FALSE;
        }
    }
    return TRUE;
}
// 为指定令牌启用特定权限，并保存原有权限以便恢复
static BOOL _EnablePrivilege(LPCTSTR priv, HANDLE handle, TOKEN_PRIVILEGES *privold) {
    TOKEN_PRIVILEGES tpriv;
    tpriv.PrivilegeCount = 1;
    tpriv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValue(0, priv, &tpriv.Privileges[0].Luid)) {
        return FALSE;
    }
    DWORD dsize = sizeof(TOKEN_PRIVILEGES);
    return AdjustTokenPrivileges(handle, FALSE, &tpriv, dsize, privold, &dsize);
}
// 崩溃现场不能走异步日志:LOG_ERROR 只把消息入队, _MiniDump 末尾的 TerminateProcess 一到,
// I/O 线程就再没机会刷盘, 失败原因连同队列一起蒸发。这里同步写, 且送一份进调试器
static void _dump_err(const char *what, DWORD code) {
    char buf[256];
    SNPRINTF(buf, sizeof(buf), "minidump: %s failed, err %lu.\n", what, code);
    OutputDebugStringA(buf);
    fputs(buf, stderr);
    fflush(stderr);
}
// dump 必须换个线程写。Windows x64 下 minicoro 走 MCO_USE_ASM, 切进协程时会把 TIB 里的栈边界
// 换成那 56KB 协程栈, 崩溃线程可能正跑在上面, 也可能本来就是栈溢出崩的;两种情况
// MiniDumpWriteDump 都没有足够栈可用, 只会返回 FALSE 留下个 0 字节文件。新线程拿的是完整线程栈
static DWORD WINAPI _dump_thread(LPVOID arg) {
    dump_arg *da = (dump_arg *)arg;
    MINIDUMP_EXCEPTION_INFORMATION exinfo;
    exinfo.ThreadId = da->da_tid;
    exinfo.ExceptionPointers = da->da_excep;
    exinfo.ClientPointers = FALSE;
    da->da_ok = MiniDumpWriteDump(GetCurrentProcess(),
                                  GetCurrentProcessId(),
                                  da->da_file,
                                  (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                                  &exinfo,
                                  NULL,
                                  NULL);
    if (!da->da_ok) {
        da->da_err = ERRNO;
    }
    return 0;
}
// Windows 结构化异常处理函数，捕获崩溃时生成 MiniDump 文件
static LONG __stdcall _MiniDump(struct _EXCEPTION_POINTERS *excep) {
    char acdmp[PATH_LENS];
    SNPRINTF(acdmp, sizeof(acdmp), "%s%s%"PRIu64"_%d.dmp",
        procpath(), PATH_SEPARATORSTR, nowsec(), (int32_t)ATOMIC_ADD_RELAXED(&_exindex, 1));
    HANDLE ptoken = NULL;
    if (!_GetImpersonationToken(&ptoken)) {
        _dump_err("OpenThreadToken", ERRNO);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HANDLE pdmpfile = CreateFile(acdmp,
                                 GENERIC_WRITE,
                                 FILE_SHARE_WRITE,
                                 NULL,
                                 CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL,
                                 NULL);
    if (INVALID_HANDLE_VALUE == pdmpfile) {
        _dump_err("CreateFile", ERRNO);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG lrtn = EXCEPTION_CONTINUE_SEARCH;
    TOKEN_PRIVILEGES tprivold;
    dump_arg da;
    da.da_ok = FALSE;
    da.da_err = ERROR_SUCCESS;
    da.da_tid = GetCurrentThreadId();
    da.da_file = pdmpfile;
    da.da_excep = excep;
    BOOL bprienabled = _EnablePrivilege(SE_DEBUG_NAME, ptoken, &tprivold);
    HANDLE hdump = CreateThread(NULL, 0, _dump_thread, &da, 0, NULL);
    if (NULL == hdump) {
        _dump_err("CreateThread", ERRNO);
    } else {
        WaitForSingleObject(hdump, INFINITE);
        CloseHandle(hdump);
        if (da.da_ok) {
            lrtn = EXCEPTION_EXECUTE_HANDLER;
        } else {
            _dump_err("MiniDumpWriteDump", da.da_err);
        }
    }
    if (bprienabled) {
        (void)AdjustTokenPrivileges(ptoken, FALSE, &tprivold, 0, NULL, NULL);
    }
    CloseHandle(pdmpfile);
    TerminateProcess(GetCurrentProcess(), 0);
    return lrtn;
}
#else
// 软上限抬到 want，硬上限比 want 低就只抬到硬上限；软上限已经够了不动，硬上限从不改。
// 返回之后的软上限，取不到上限时返回 want(错误已打过)
static rlim_t _rlimit_raise(int32_t resource, rlim_t want) {
    struct rlimit rl;
    rlim_t cur;
    if (ERR_OK != getrlimit(resource, &rl)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return want;
    }
    cur = rl.rlim_cur;
    if (want > rl.rlim_max) {
        want = rl.rlim_max;
    }
    if (cur >= want) {
        return cur;
    }
    rl.rlim_cur = want;
    if (ERR_OK != setrlimit(resource, &rl)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return cur;
    }
    return want;
}
#endif
void unlimit(void) {
#ifdef OS_WIN
    SetUnhandledExceptionFilter(_MiniDump);
#else
    rlim_t nofile, got;
#ifdef HAVE_BACKTRACE
    void *frame;
    // glibc 首次调 backtrace 要加载 libgcc_s，会分配内存。先调一次，OOM 时 stack_print 才打得出栈
    (void)backtrace(&frame, 1);
#endif
    (void)_rlimit_raise(RLIMIT_CORE, RLIM_INFINITY);
#ifdef OS_DARWIN
    nofile = OPEN_MAX;// macOS 的 NOFILE 软上限不接受 RLIM_INFINITY，最多 OPEN_MAX
#else
    nofile = 65535;
#endif
    got = _rlimit_raise(RLIMIT_NOFILE, nofile);
    if (got < nofile) {
        LOG_WARN("max open files is %" PRIu64 ", capped by the hard limit; raise it (ulimit -Hn / LimitNOFILE) for more connections.",
                 (uint64_t)got);
    }
#endif
}
#ifdef OS_WIN
// Windows 控制台事件处理回调，将控制台信号转发给用户注册的处理函数
static BOOL WINAPI _sighandler(DWORD dsig) {
    switch (dsig) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        _sig_cb((int32_t)dsig, _ud);
        break;
    }
    return TRUE;
}
#else
// POSIX 信号处理回调，将系统信号转发给用户注册的处理函数
static void _sighandler(int32_t isig) {
    _sig_cb(isig, _ud);
}
#endif
void sighandle(void(*cb)(int32_t, void *), void *data) {
    _ud = data;
    _sig_cb = cb;
#ifdef OS_WIN
    (void)SetConsoleCtrlHandler((PHANDLER_ROUTINE)_sighandler, TRUE);
#else
    signal(SIGPIPE, SIG_IGN);//忽略 SIGPIPE：对端关闭后继续写入不会导致进程崩溃
    signal(SIGHUP, _sighandler);//终端断开或控制进程退出
    signal(SIGINT, _sighandler);//键盘中断（Ctrl-C）
    signal(SIGQUIT, _sighandler);//键盘退出（Ctrl-\）
    signal(SIGABRT, _sighandler);//异常中止（abort）
    signal(SIGTSTP, _sighandler);//终端暂停（Ctrl-Z）
    /* signal(SIGKILL, ...) 无效：SIGKILL 由内核保留，不可捕获/忽略，OS 静默丢弃该调用 */
    signal(SIGTERM, _sighandler);//正常终止进程
    signal(SIGUSR1, _sighandler);
    signal(SIGUSR2, _sighandler);
#endif
}
uint64_t threadid(void) {
#if defined(OS_WIN)
    return (uint64_t)GetCurrentThreadId();
#else
    return (uint64_t)pthread_self();
#endif
}
uint32_t _cgroup_cpu_quota(const char *text) {
    uint64_t quota, period;
    if (2 != sscanf(text, "%" SCNu64 " %" SCNu64, &quota, &period)
        || 0 == period) {
        return 0;
    }
    quota = quota / period + (0 != quota % period);
    return quota > UINT32_MAX ? UINT32_MAX : (uint32_t)quota;
}
#if defined(OS_LINUX)
// n 与 cgroup v2 的 CPU 配额(cpu.max 折成核数，向上取整)取小。从本进程所在组逐级往上读，取最紧的一级；
// 只认挂在 /sys/fs/cgroup 的 v2，文件读不到、"max"(不限)或解析不了的那级一律当不限
static uint32_t _cgroup_cpus(uint32_t n) {
    char line[PATH_LENS];
    char file[PATH_LENS];
    char val[64];
    char *path = NULL;
    char *slash;
    FILE *fp;
    uint32_t quota;
    fp = fopen_cloexec("/proc/self/cgroup", "r");
    if (NULL == fp) {
        return n;
    }
    while (NULL != fgets(line, sizeof(line), fp)) {
        if (0 == strncmp(line, "0::/", 4)) {// v2 那行形如 "0::/system.slice/x.service"
            path = line + 3;
            break;
        }
    }
    fclose(fp);
    if (NULL == path) {
        return n;
    }
    path[strcspn(path, "\n")] = '\0';
    do {
        if (SNPRINTF(file, sizeof(file), "/sys/fs/cgroup%s/cpu.max", path) < (int)sizeof(file)
            && NULL != (fp = fopen_cloexec(file, "r"))) {
            if (NULL != fgets(val, sizeof(val), fp)) {
                quota = _cgroup_cpu_quota(val);
                if (0 != quota
                    && quota < n) {
                    n = quota;
                }
            }
            fclose(fp);
        }
        slash = strrchr(path, '/');
        if (NULL != slash) {
            *slash = '\0';
        }
    } while (NULL != slash);
    return n;
}
#endif
uint32_t procscnt(void) {
    static atomic_t cnt = 0;
    uint32_t n = (uint32_t)ATOMIC_GET_RELAXED(&cnt);
    if (0 != n) {
        return n;
    }
#if defined(OS_WIN)
    SYSTEM_INFO stinfo;
    GetSystemInfo(&stinfo);
    n = (uint32_t)stinfo.dwNumberOfProcessors;
#else
    n = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
#endif
    ATOMIC_SET_RELAXED(&cnt, n);
    return n;
}
uint32_t procscnt_avail(void) {
    static atomic_t cnt = 0;
    uint32_t n = (uint32_t)ATOMIC_GET_RELAXED(&cnt);
    if (0 != n) {
        return n;
    }
#if defined(OS_LINUX)
    cpu_set_t cpus;
    if (0 == sched_getaffinity(0, sizeof(cpus), &cpus)) {
        n = (uint32_t)CPU_COUNT(&cpus);
    } else {
        n = procscnt();// 超过 CPU_SETSIZE(1024)核时取不到亲和
    }
    n = _cgroup_cpus(n);
#elif defined(OS_FBSD)
    cpuset_t cpus;
    if (0 == cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, sizeof(cpus), &cpus)) {
        n = (uint32_t)CPU_COUNT(&cpus);
    } else {
        n = procscnt();
    }
#else
    n = procscnt();
#endif
    ATOMIC_SET_RELAXED(&cnt, n);
    return n;
}
#ifdef OS_AIX
// AIX 平台：通过 getprocs 遍历进程列表，查找指定 pid 的进程信息
static int32_t _get_proc(pid_t pid, struct procsinfo *info) {
    int32_t i, cnt;
    pid_t index = 0;
    struct procsinfo pinfo[16];
    while ((cnt = getprocs(pinfo, sizeof(struct procsinfo), NULL, 0, &index, 16)) > 0) {
        for (i = 0; i < cnt; i++) {
            if (SZOMB == pinfo[i].pi_state) {
                continue;
            }
            //pinfo[i].pi_comm 为程序名称
            if (pid == pinfo[i].pi_pid) {
                memcpy(info, &pinfo[i], sizeof(struct procsinfo));
                return ERR_OK;
            }
        }
    }
    return ERR_FAILED;
}
// AIX 平台：获取指定 pid 进程的可执行文件完整路径
static int32_t _get_proc_fullpath(pid_t pid, char path[PATH_LENS]) {
    struct procsinfo pinfo;
    if (ERR_OK != _get_proc(pid, &pinfo)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    char args[ONEK];
    //args 由连续以 '\0' 结尾的字符串组成，两个连续 NULL 表示列表结束
    if (ERR_OK != getargs(&pinfo, sizeof(struct procsinfo), args, sizeof(args))) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    if (NULL == realpath(args, path)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
#endif
// 跨平台获取当前可执行文件所在目录路径（末尾不含斜杠）。
// GetModuleFileName 与 readlink 装不下时都不报错、只把结果截断（前者在 XP 上还不补 NUL，
// 拿去 strrchr 会越界读），故这两支必须连同"返回值顶到缓冲大小"一起判失败
static int32_t _get_procpath(char path[PATH_LENS]) {
#ifndef OS_AIX
    size_t len = PATH_LENS;
#endif
#if defined(OS_WIN)
    DWORD wlen = GetModuleFileName(NULL, path, (DWORD)len);
    if (0 == wlen
        || wlen >= len) {
        return ERR_FAILED;
    }
#elif defined(OS_LINUX) || defined(OS_NBSD) || defined(OS_DFBSD) || defined(OS_SUN)
  #if defined(OS_SUN)
    char link[64];
    SNPRINTF(link, sizeof(link), "/proc/%d/path/a.out", (int32_t)GETPID());
  #elif defined(OS_LINUX)
    const char *link = "/proc/self/exe";
  #elif defined(OS_NBSD)
    const char *link = "/proc/curproc/exe";
  #elif defined(OS_DFBSD)
    const char *link = "/proc/curproc/file";
  #else
    // 新平台加进上面的 #elif 条件时，这里也要补它自己的 symlink 路径
    #error "_get_procpath: add the symlink path for this platform"
  #endif
    ssize_t rlen = readlink(link, path, len - 1);
    if (0 > rlen
        || (size_t)rlen >= len - 1) {
        return ERR_FAILED;
    }
    path[rlen] = '\0';
#elif defined(OS_DARWIN)
    uint32_t umaclens = len;
    if (0 != _NSGetExecutablePath(path, &umaclens)) {
        return ERR_FAILED;
    }
#elif defined(OS_FBSD)
    int32_t name[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME };
    name[3] = GETPID();
    if (0 != sysctl(name, 4, path, &len, NULL, 0)) {
        return ERR_FAILED;
    }
#elif defined(OS_AIX)
    if (ERR_OK != _get_proc_fullpath(GETPID(), path)) {
        return ERR_FAILED;
    }
#elif defined(OS_HPUX)
    struct pst_status pst;
    if (-1 == pstat_getproc(&pst, sizeof(pst), 0, GETPID())) {
        return ERR_FAILED;
    }
    if (-1 == pstat_getpathname(path, len - 1, &pst.pst_fid_text)) {
        return ERR_FAILED;
    }
#else
#error "not support."
#endif
    char* cur = strrchr(path, PATH_SEPARATOR);
    if (NULL == cur) {
        return ERR_FAILED;
    }
    *cur = 0;
#if defined(OS_DARWIN)
    cur = (0 == strncmp(path, "./", 2)) ? path : NULL;
    if (NULL != cur) {
        len = strlen(cur + 2);
        memmove(path + (cur - path), cur + 2, len);
        len = cur - path + len;
        path[len] = 0;
    } else {
        len = strlen(path);
        if (len >= 2
            && '.' == path[len - 1]
            && PATH_SEPARATOR == path[len - 2]) {
            path[len - 2] = 0;
        }
    }
    if (PATH_SEPARATOR == path[0]
        && PATH_SEPARATOR == path[1]) {
        len = strlen(path);
        memmove(path, path + 1, len - 1);
        path[len - 1] = 0;
    }
#endif
    return ERR_OK;
}
const char *procpath(void) {
    if (once_begin(&_path_once)) {
        ASSERTAB(ERR_OK == _get_procpath(_path), ERRORSTR(ERRNO));
        once_end(&_path_once);
    }
    return _path;
}
