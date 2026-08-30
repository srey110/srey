#include "utils/utils.h"
#include "utils/strptime.h"
#include "base/structs.h"
#include <locale.h>
#if defined(OS_DARWIN) || defined(OS_BSD)
    #include <xlocale.h>
#endif

#ifdef OS_WIN
#pragma comment(lib, "Dbghelp.lib" )
#pragma comment(lib, "Bcrypt.lib")
static atomic_t _exindex = 0;
static _locale_t g_numeric_c;
#else
static locale_t g_numeric_c;
#endif

#if defined(OS_WIN)
// 只有 _now_usec 用;这几条在 macOS/Linux 上从不编译, 改了没有本机回归网兜着
#define U64_LITERAL(n) n##ui64
#define EPOCH_BIAS U64_LITERAL(116444736000000000) //Windows FILETIME 纪元与 Unix 纪元的差值（100ns 单位）
#define UNITS_PER_USEC U64_LITERAL(10)//每微秒的 100ns 单位数
// FILETIME 与 uint64 共用同一块内存:GetSystemTimeAsFileTime 写前者,算术走后者
typedef union filetime_u64 {
    FILETIME ft_ft;
    uint64_t ft_64;
}filetime_u64;
#endif
// tchar 集合见 RFC 7230 §3.2.6：ALPHA / DIGIT / "!#$%&'*+-.^_`|~" 为 1，其余一概为 0。
// 按 16 列排，行首注释是高 4 位
static const uint8_t TCHAR_TBL[256] = {
    /* 0x0 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x1 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x2 */ 0,1,0,1,1,1,1,1,0,0,1,1,0,1,1,0,
    /* 0x3 */ 1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,
    /* 0x4 */ 0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    /* 0x5 */ 1,1,1,1,1,1,1,1,1,1,1,0,0,0,1,1,
    /* 0x6 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    /* 0x7 */ 1,1,1,1,1,1,1,1,1,1,1,0,1,0,1,0,
    /* 0x8 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x9 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xA */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xB */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xC */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xD */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xE */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0xF */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
#define _FMT_STACK_SIZE 512
static void *_ud;//信号处理回调的用户数据
static void(*_sig_cb)(int32_t, void *);//用户注册的信号处理回调函数
static uint16_t _serviceid = 1;
static atomic64_t _ids = 1;//全局自增 ID 原子计数器
static char _path[PATH_LENS] = { 0 };//程序所在目录路径缓存
static atomic_t _path_once = 0;//路径初始化状态：0=未初始化 1=初始化中 2=已完成

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
// Windows 结构化异常处理函数，捕获崩溃时生成 MiniDump 文件
static LONG __stdcall _MiniDump(struct _EXCEPTION_POINTERS *excep) {
    char acdmp[PATH_LENS];
    SNPRINTF(acdmp, sizeof(acdmp), "%s%s%lld_%d.dmp",
        procpath(), PATH_SEPARATORSTR, nowsec(), (int32_t)ATOMIC_ADD(&_exindex, 1));
    HANDLE ptoken = NULL;
    if (!_GetImpersonationToken(&ptoken)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
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
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG lrtn = EXCEPTION_CONTINUE_SEARCH;
    TOKEN_PRIVILEGES tprivold;
    MINIDUMP_EXCEPTION_INFORMATION exinfo;
    exinfo.ThreadId = GetCurrentThreadId();
    exinfo.ExceptionPointers = excep;
    exinfo.ClientPointers = FALSE;
    BOOL bprienabled = _EnablePrivilege(SE_DEBUG_NAME, ptoken, &tprivold);
    BOOL bok = MiniDumpWriteDump(GetCurrentProcess(),
                                 GetCurrentProcessId(),
                                 pdmpfile,
                                 MiniDumpNormal,
                                 &exinfo,
                                 NULL,
                                 NULL);
    if (bok) {
        lrtn = EXCEPTION_EXECUTE_HANDLER;
    } else {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
    if (bprienabled) {
        (void)AdjustTokenPrivileges(ptoken, FALSE, &tprivold, 0, NULL, NULL);
    }
    CloseHandle(pdmpfile);
    TerminateProcess(GetCurrentProcess(), 0);
    return lrtn;
}
#endif
void unlimit(void) {
#ifdef OS_WIN
    SetUnhandledExceptionFilter(_MiniDump);
#else
    struct rlimit stnew;
    stnew.rlim_cur = stnew.rlim_max = RLIM_INFINITY;
    if (ERR_OK != setrlimit(RLIMIT_CORE, &stnew)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
#ifdef OS_DARWIN
    rlim_t rlmax = OPEN_MAX;
#else
    rlim_t rlmax = 65535;
#endif
    stnew.rlim_cur = stnew.rlim_max = rlmax;
    if (ERR_OK != setrlimit(RLIMIT_NOFILE, &stnew)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
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
int32_t serviceid(uint16_t id) {
    if (id >= 0x8000) {
        return ERR_FAILED;
    }
    _serviceid = id;
    return ERR_OK;
}
uint64_t createid(void) {
    return ((uint64_t)_serviceid << 48) | ((uint64_t)ATOMIC64_ADD(&_ids, 1) & 0xFFFFFFFFFFFFULL);
}
uint16_t parse_svid(uint64_t id) {
    return (uint16_t)(id >> 48);
}
uint64_t threadid(void) {
#if defined(OS_WIN)
    return (uint64_t)GetCurrentThreadId();
#else
    return (uint64_t)pthread_self();
#endif
}
uint32_t procscnt(void) {
#if defined(OS_WIN)
    SYSTEM_INFO stinfo;
    GetSystemInfo(&stinfo);
    return (uint32_t)stinfo.dwNumberOfProcessors;
#else
    return (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
#endif
}
int32_t isfile(const char *file) {
    struct FSTAT st;
    if (ERR_OK != FSTAT(file, &st)) {
        return ERR_FAILED;
    }
#if defined(OS_WIN)
    if (BIT_CHECK(st.st_mode, _S_IFREG)) {
        return ERR_OK;
    }
    return ERR_FAILED;
#else    
    return S_ISREG(st.st_mode) ? ERR_OK : ERR_FAILED;
#endif    
}
int32_t isdir(const char *path) {
    struct FSTAT st;
    if (ERR_OK != FSTAT(path, &st)) {
        return ERR_FAILED;
    }
#if defined(OS_WIN)
    if (BIT_CHECK(st.st_mode, _S_IFDIR)) {
        return ERR_OK;
    }
    return ERR_FAILED;
#else
    return S_ISDIR(st.st_mode) ? ERR_OK : ERR_FAILED;
#endif
}
int64_t filesize(const char *file) {
    struct FSTAT st;
    if (ERR_OK != FSTAT(file, &st)) {
        return ERR_FAILED;
    }
    return st.st_size;
}
uint64_t file_mtime(const char *file) {
    struct FSTAT st;
    if (ERR_OK != FSTAT(file, &st)) {
        return 0;
    }
    return (uint64_t)st.st_mtime;
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
// 跨平台获取当前可执行文件所在目录路径（末尾不含斜杠）
static int32_t _get_procpath(char path[PATH_LENS]) {
#ifndef OS_AIX
    size_t len = PATH_LENS;
#endif
#if defined(OS_WIN)
    if (0 == GetModuleFileName(NULL, path, (DWORD)len - 1)) {
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
    #error "_get_procpath: 新平台加进上面的 #elif 条件时，这里也要补它自己的 symlink 路径"
  #endif
    ssize_t rlen = readlink(link, path, len - 1);
    if (0 > rlen) {
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
    if (ATOMIC_CAS(&_path_once, 0, 1)) {
        /* 赢得 CAS(0→1)：唯一写者，填充 _path */
        ASSERTAB(ERR_OK == _get_procpath(_path), ERRORSTR(ERRNO));
        /* CAS(1→2) 作为 release 屏障：_path 的所有写入在状态变为 2 之前对其他核可见 */
        ATOMIC_CAS(&_path_once, 1, 2);
    } else {
        /* ATOMIC_GET acquire 自旋：等写者将状态置为 2，acquire 语义保证读到完整 _path */
        while (2 != ATOMIC_GET(&_path_once)) {
            CPU_PAUSE();
        }
    }
    return _path;
}
char *readall(const char *file, size_t *lens) {
    FILE *fp = fopen(file, "rb");
    if (NULL == fp) {
        return NULL;
    }
    if (0 != fseek(fp, 0, SEEK_END)) {
        /* fclose 失败会盖掉 fseek 留下的 errno，先存后还 */
        int32_t err = errno;
        fclose(fp);
        errno = err;
        return NULL;
    }
    long sz = ftell(fp);
    /* ftell 对管道/特殊文件返回 -1，文件大小 0 也视为无效。这条不来自失败的系统调用，
       errno 里躺着的是上一次调用的陈旧值，得自己补一个 */
    if (sz <= 0) {
        fclose(fp);
        errno = EINVAL;
        return NULL;
    }
    rewind(fp);
    char *buf;
    MALLOC(buf, (size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        /* 短读：文件在读取过程中被截断或发生 I/O 错误；fread 不保证置 errno，同上自己补 */
        FREE(buf);
        errno = EIO;
        return NULL;
    }
    buf[sz] = '\0';
    *lens = (size_t)sz;
    return buf;
}
int32_t timeoffset(void) {
    time_t now = time(NULL);
    struct tm gmt_tm, loc_tm;
    if (0 != GMTIME(&now, &gmt_tm)) {
        return 0;
    }
    time_t gt = mktime(&gmt_tm);
    if ((time_t)-1 == gt
        || 0 != LOCALTIME(&gt, &loc_tm)) {
        return 0;
    }
    return ((int32_t)(now - gt) + (loc_tm.tm_isdst ? 3600 : 0)) / 60;
}
// Unix 纪元起的微秒数,全程 64 位。timeofday 反过来由它推导:Windows 的 struct timeval.tv_sec
// 是 32 位 long(LLP64),2038-01-19 后回绕为负,再转 uint64 会符号扩展成约 1.8e19
static uint64_t _now_usec(void) {
#if defined(OS_WIN)
    filetime_u64 ft;
    GetSystemTimeAsFileTime(&ft.ft_ft);
    ft.ft_64 -= EPOCH_BIAS;
    return ft.ft_64 / UNITS_PER_USEC;
#else
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
#endif
}
uint64_t nowms(void) {
    return _now_usec() / 1000;
}
uint64_t nowsec(void) {
    return _now_usec() / 1000000;
}
void timeofday(struct timeval *tv) {
    uint64_t us = _now_usec();
    tv->tv_sec = (long)(us / 1000000);
    tv->tv_usec = (long)(us % 1000000);
}
// 秒级格式化, 成功时回填写入长度供调用方接着写后缀; 两处失败都把 time 置空串
static int32_t _sectostr_lens(uint64_t sec, const char *fmt, char time[TIME_LENS], size_t *lens) {
    time_t t = (time_t)sec;
    struct tm loc_tm;
    // 失败时 loc_tm 未必被写过,交给 strftime 就是拿不定的 tm_wday/tm_mon 去索引 libc 的静态名表
    if (0 != LOCALTIME(&t, &loc_tm)) {
        time[0] = '\0';
        return ERR_FAILED;
    }
    *lens = strftime(time, TIME_LENS - 1, fmt, &loc_tm);
    if (0 == *lens) {
        time[0] = '\0';
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t sectostr(uint64_t sec, const char *fmt, char time[TIME_LENS]) {
    size_t lens;
    return _sectostr_lens(sec, fmt, time, &lens);
}
int32_t mstostr(uint64_t ms, const char *fmt, char time[TIME_LENS]) {
    size_t lens;
    if (ERR_OK != _sectostr_lens(ms / 1000, fmt, time, &lens)) {
        return ERR_FAILED;
    }
    // 装不下就整体失败,截断的时间串拿去用是静默出错
    const size_t mslens = sizeof(" 000");
    if (TIME_LENS - lens < mslens) {
        time[0] = '\0';
        return ERR_FAILED;
    }
    SNPRINTF(time + lens, TIME_LENS - lens, " %03d", (int32_t)(ms % 1000));
    return ERR_OK;
}
uint64_t strtots(const char *time, const char *fmt) {
    struct tm dttm = { 0 };
    if (NULL == _strptime(time, fmt, &dttm)) {
        return 0;
    }
    time_t ts = mktime(&dttm);
    if ((time_t)-1 == ts) {
        return 0;
    }
    return (uint64_t)ts;
}
uint64_t hash(const char *buf, size_t len) {
    uint64_t rtn = 0;
    for (; len > 0; --len) {
        rtn = (rtn * 131) + (unsigned char)*buf++;
    }
    return rtn;
}
int32_t is_token(const char *data, size_t lens) {
    unsigned char c;
    size_t i;
    if (0 == lens
        || NULL == data) {
        return 0;
    }
    for (i = 0; i < lens; i++) {
        c = (unsigned char)data[i];
        if (!TCHAR_TBL[c]) {
            return 0;
        }
    }
    return 1;
}
void locale_init(void) {
#ifdef OS_WIN
    g_numeric_c = _create_locale(LC_NUMERIC, "C");
    ASSERTAB(NULL != g_numeric_c, "_create_locale(LC_NUMERIC, \"C\") failed.");
#else
    g_numeric_c = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    ASSERTAB((locale_t)0 != g_numeric_c, ERRORSTR(ERRNO));
#endif
}
void locale_free(void) {
#ifdef OS_WIN
    if (NULL != g_numeric_c) {
        _free_locale(g_numeric_c);
        g_numeric_c = NULL;
    }
#else
    if ((locale_t)0 != g_numeric_c) {
        freelocale(g_numeric_c);
        g_numeric_c = (locale_t)0;
    }
#endif
}
double strtod_c(const char *str, char **endptr) {
#ifdef OS_WIN
    return _strtod_l(str, endptr, g_numeric_c);
#else
    return strtod_l(str, endptr, g_numeric_c);
#endif
}
// xorshift64* 伪随机数生成器，线程局部状态，首次调用自动用线程ID+时间戳初始化种子
static uint64_t _xorshift64(void) {
    static THREAD_LOCAL uint64_t _tls_rand = 0;
    if (0 == _tls_rand) {
        /* 首次调用：用线程 ID 与时间戳组合初始化种子，避免种子为 0 */
        _tls_rand = (uint64_t)threadid() ^ (nowms() * 6364136223846793005ULL + 1442695040888963407ULL);
        if (0 == _tls_rand){
            _tls_rand = 1;
        }
    }
    uint64_t x = _tls_rand;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    _tls_rand = x;
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
// buf 必须至少分配 len+1 字节；函数在 buf[len] 处写 '\0'
char *randstr(char *buf, size_t len) {
    static char characters[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U',
        'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
        'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };
    size_t i = 0;
    for (; i < len; i++) {
        buf[i] = characters[randrange(0, sizeof(characters) - 1)];
    }
    buf[i] = '\0';
    return buf;
}
// 按 flags 预处理一段:SPLIT_TRIM 剔两端 OWS,SPLIT_SKIPEMPTY 丢空段。
// trim 必须排在判空之前——" " 这种全空白段原始长度是 1 不是 0,先判空就漏过去了。
// 返回 0 表示本段应跳过;返回非 0 时 *data / *lens 已是处理后的值
static int32_t _split_filter(char **data, size_t *lens, int32_t flags) {
    if (BIT_CHECK(flags, SPLIT_TRIM)) {
        size_t tlens = 0;
        char *tdata = trim(*data, *lens, &tlens);
        if (NULL != tdata) {
            *data = tdata;
        }
        *lens = tlens;
    }
    return !(BIT_CHECK(flags, SPLIT_SKIPEMPTY) && 0 == *lens);
}
// 写一段到 (*segs)[*n]。堆模式(heap 非 0)容量不够就翻倍并回写 *segs;
// 栈模式满了看 SPLIT_TRUNCATE:开了返 1 让调用方就此收尾,没开返 ERR_FAILED
static int32_t _split_store(buf_ctx **segs, int32_t *n, size_t *total, int32_t heap,
                            int32_t flags, char *data, size_t lens) {
    if ((size_t)*n >= *total) {
        if (0 == heap) {
            if (BIT_CHECK(flags, SPLIT_TRUNCATE)) {
                return 1;
            }
            LOG_WARN("split segments exceed cap.");
            return ERR_FAILED;
        }
        *total *= 2;// 无符号翻倍;段数上限由下面的 INT32_MAX 判定负责,这里不会先回绕
        REALLOC(*segs, *segs, sizeof(buf_ctx) * (*total));
    }
    if (INT32_MAX == *n) {
        LOG_WARN("split segments exceed INT32_MAX.");
        return ERR_FAILED;
    }
    (*segs)[*n].data = data;
    (*segs)[*n].lens = lens;
    (*n)++;
    return ERR_OK;
}
int32_t split(char *ptr, size_t plens, const char *sep, size_t seplens,
              buf_ctx **segs, int32_t cap, int32_t flags) {
    // plens 为 0 不算错:按"段数 = sep 出现次数 + 1"该出一个空段,url_parse 解 "/" 时正靠这个
    if (NULL == ptr
        || NULL == segs
        || cap < 0
        || (cap > 0 && NULL == *segs)) {
        return ERR_FAILED;
    }
    int32_t heap = (0 == cap);
    size_t total = heap ? 32 : (size_t)cap;
    if (0 != heap) {
        MALLOC(*segs, sizeof(buf_ctx) * total);
    }
    int32_t n = 0;
    int32_t rtn;
    char *cur = ptr;
    char *pos;
    char *data;
    size_t remain = plens;
    size_t slen;
    size_t lens;
    for (;;) {
        // sep 为空即整段不切。单字节直接 memchr:memstr 那条要过 mem_funcs_pick 加两次
        // 间接调用,而 url_parse 每个请求都要切一次路径
        if (NULL == sep
            || 0 == seplens) {
            pos = NULL;
        } else if (1 == seplens) {
            pos = memchr(cur, (uint8_t)sep[0], remain);
        } else {
            pos = memstr(0, cur, remain, sep, seplens);
        }
        slen = (NULL != pos) ? (size_t)(pos - cur) : remain;
        data = cur;
        lens = slen;
        if (0 != _split_filter(&data, &lens, flags)) {
            rtn = _split_store(segs, &n, &total, heap, flags, data, lens);
            if (ERR_FAILED == rtn) {
                return ERR_FAILED;
            }
            if (ERR_OK != rtn) {
                break;// 栈模式截断
            }
        }
        // 尾随分隔符不用特判:remain 归 0 后再走一轮,memchr/memstr 对长度 0 都返 NULL,
        // 自然补出那个空段(段数 = sep 出现次数 + 1)
        if (NULL == pos) {
            break;
        }
        remain -= (slen + seplens);
        cur = pos + seplens;
    }
    return n;
}
char *_format_va(const char *fmt, va_list args) {
    /* 先用栈缓冲尝试格式化（绝大多数场景够用），成功则直接复制返回，避免堆分配；
     * 仅当字符串超过栈缓冲大小时，才按实际长度堆分配并重试。 */
    char stk[_FMT_STACK_SIZE];
    va_list args2;
    va_copy(args2, args);
    int32_t rtn = vsnprintf(stk, _FMT_STACK_SIZE, fmt, args);
    if (rtn < 0) {
        va_end(args2);
        return NULL;
    }
    if (rtn < _FMT_STACK_SIZE) {
        va_end(args2);
        return dup_zero(stk, (size_t)rtn);
    }
    /* 栈缓冲不足，按实际长度堆分配后重试 */
    size_t size = (size_t)rtn + 1;
    char *pbuff;
    MALLOC(pbuff, size);
    rtn = vsnprintf(pbuff, size, fmt, args2);
    va_end(args2);
    if (rtn < 0) {
        FREE(pbuff);
        return NULL;
    }
    return pbuff;
}
char *format_va(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *buf = _format_va(fmt, args);
    va_end(args);
    return buf;
}
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_BSD)
// 反复取直到填满：一次调用未必给够，EINTR 之类可重试错误继续，其余即失败。
// Linux 走 getrandom(2) 不用 fd，其余 Unix 从 /dev/urandom 的 fd 读
static int32_t _rand_drain(void *buf, size_t len, int32_t fd) {
    size_t got = 0;
    ssize_t ret;
    (void)fd;
    while (got < len) {
#if defined(OS_LINUX)
        ret = syscall(SYS_getrandom, (char *)buf + got, len - got, 0);
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
#endif
int32_t csprng_rand(void *buf, size_t len) {
#if defined(OS_WIN)
    /* Windows：BCryptGenRandom 使用系统首选 CSPRNG，不依赖进程安全句柄。*/
    if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return ERR_FAILED;
    }
    return ERR_OK;
#elif defined(OS_DARWIN) || defined(OS_BSD)
    /* Darwin / BSD（macOS、FreeBSD、NetBSD、OpenBSD、DragonFly）：
     * arc4random_buf 由内核 CSPRNG 支撑，永不失败，无需检查返回值。*/
    arc4random_buf(buf, len);
    return ERR_OK;
#elif defined(OS_LINUX)
    /* Linux：getrandom(2) 系统调用（内核 3.17+），阻塞直至熵池就绪。*/
    return _rand_drain(buf, len, -1);
#else
    /* 其余 Unix（Solaris、AIX、HP-UX 等）读 /dev/urandom, 缓存 fd 省掉每次 open+close。
     * 存的是 fd+1: 0 表示未初始化, 否则真 fd = 值-1 —— daemon 关掉 stdin 后 fd 会是 0,
     * 不加偏移就分不清"没初始化"和"fd 就是 0"。fd 长期持有, 进程退出交给 OS 清理。*/
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
#endif
}
