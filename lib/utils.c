#include "utils.h"

#ifdef OS_WIN
#pragma warning(disable:4091)
#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib" )
static atomic_t _exindex = 0;
#endif

static void *_ud;
static void(*_sig_cb)(int32_t, void *);
static atomic64_t _ids = 1;
static char _path[PATH_LENS] = { 0 };
static atomic_t _path_once = 0;

#ifdef OS_WIN
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
static LONG __stdcall _MiniDump(struct _EXCEPTION_POINTERS *excep) {
    char acdmp[PATH_LENS] = { 0 };
    SNPRINTF(acdmp, sizeof(acdmp) - 1, "%s%s%lld_%d.dmp",
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
static inline void _sighandler(int32_t isig) {
    _sig_cb(isig, _ud);
}
#endif
void sighandle(void(*cb)(int32_t, void *), void *data) {
    _ud = data;
    _sig_cb = cb;
#ifdef OS_WIN
    (void)SetConsoleCtrlHandler((PHANDLER_ROUTINE)_sighandler, TRUE);
#else
    signal(SIGPIPE, SIG_IGN);//若某一端关闭连接，而另一端仍然向它写数据，第一次写数据后会收到RST响应，此后再写数据，内核将向进程发出SIGPIPE信号
    signal(SIGHUP, _sighandler);//终止控制终端或进程
    signal(SIGINT, _sighandler);//键盘产生的中断(Ctrl-C)
    signal(SIGQUIT, _sighandler);//键盘产生的退出
    signal(SIGABRT, _sighandler);//异常中止
    signal(SIGTSTP, _sighandler);//ctrl+Z
    signal(SIGKILL, _sighandler);//立即结束程序
    signal(SIGTERM, _sighandler);//进程终止
    signal(SIGUSR1, _sighandler);
    signal(SIGUSR2, _sighandler);
#endif
}
uint64_t createid(void) {
    return ATOMIC64_ADD(&_ids, 1);
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
#ifdef OS_AIX
static int32_t _get_proc(pid_t pid, struct procsinfo *info) {
    int32_t i, cnt;
    pid_t index = 0;
    struct procsinfo pinfo[16];
    while ((cnt = getprocs(pinfo, sizeof(struct procsinfo), NULL, 0, &index, 16)) > 0) {
        for (i = 0; i < cnt; i++) {
            if (SZOMB == pinfo[i].pi_state) {
                continue;
            }
            //pinfo[i].pi_comm 程序名
            if (pid == pinfo[i].pi_pid) {
                memcpy(info, &pinfo[i], sizeof(struct procsinfo));
                return ERR_OK;
            }
        }
    }
    return ERR_FAILED;
}
static int32_t _get_proc_fullpath(pid_t pid, char path[PATH_LENS]) {
    struct procsinfo pinfo;
    if (ERR_OK != _get_proc(pid, &pinfo)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    char args[ONEK];
    //args consists of a succession of strings, each terminated with a null character (ascii `\0'). 
    //Hence, two consecutive NULLs indicate the end of the list.
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
static inline int32_t _get_procpath(char path[PATH_LENS]) {
#ifndef OS_AIX
    size_t len = PATH_LENS;
#endif
#if defined(OS_WIN)
    if (0 == GetModuleFileName(NULL, path, (DWORD)len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_LINUX)
    if (0 > readlink("/proc/self/exe", path, len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_NBSD)
    if (0 > readlink("/proc/curproc/exe", path, len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_DFBSD)
    if (0 > readlink("/proc/curproc/file", path, len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_SUN)  
    char in[64] = { 0 };
    SNPRINTF(in, sizeof(in) - 1, "/proc/%d/path/a.out", (uint32_t)getpid());
    if (0 > readlink(in, path, len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_DARWIN)
    uint32_t umaclens = len;
    if (0 != _NSGetExecutablePath(path, &umaclens)) {
        return ERR_FAILED;
    }
#elif defined(OS_FBSD)
    int32_t name[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME };
    name[3] = getpid();
    if (0 != sysctl(name, 4, path, &len, NULL, 0)) {
        return ERR_FAILED;
    }
#elif defined(OS_AIX)
    if (ERR_OK != _get_proc_fullpath(getpid(), path)) {
        return ERR_FAILED;
    }
#elif defined(OS_HPUX)
    struct pst_status pst;
    if (-1 == pstat_getproc(&pst, sizeof(pst), 0, getpid())) {
        return ERR_FAILED;
    }
    if (-1 == pstat_getpathname(path, len - 1, &pst.pst_fid_text)) {
        return ERR_FAILED;
    }
#else
#error "not support."
#endif
    char* cur = strrchr(path, PATH_SEPARATOR);
    *cur = 0;
#if defined(OS_DARWIN)
    cur = strstr(path, "./");
    if (NULL != cur) {
        len = strlen(cur + 2);
        memcpy(path + (cur - path), cur + 2, len);
        len = cur - path + len;
        path[len] = 0;
    } else {
        len = strlen(path);
        if ('.' == path[len - 1]
            && PATH_SEPARATOR == path[len - 2]) {
            path[len - 2] = 0;
        }
    }
    if (PATH_SEPARATOR == path[0]
        && PATH_SEPARATOR == path[1]) {
        len = strlen(path);
        memcpy(path, path + 1, len - 1);
        path[len - 1] = 0;
    }
#endif
    return ERR_OK;
}
const char *procpath(void) {
    if (ATOMIC_CAS(&_path_once, 0, 1)) {
        ASSERTAB(ERR_OK == _get_procpath(_path), ERRORSTR(ERRNO));
    }
    return _path;
}
void timeofday(struct timeval *tv) {
#if defined(OS_WIN)
#define U64_LITERAL(n) n##ui64
#define EPOCH_BIAS U64_LITERAL(116444736000000000)
#define UNITS_PER_SEC U64_LITERAL(10000000)
#define USEC_PER_SEC U64_LITERAL(1000000)
#define UNITS_PER_USEC U64_LITERAL(10)
    union {
        FILETIME ft_ft;
        uint64_t ft_64;
    } ft;
    GetSystemTimeAsFileTime(&ft.ft_ft);
    ft.ft_64 -= EPOCH_BIAS;
    tv->tv_sec = (long)(ft.ft_64 / UNITS_PER_SEC);
    tv->tv_usec = (long)((ft.ft_64 / UNITS_PER_USEC) % USEC_PER_SEC);
#else
    (void)gettimeofday(tv, NULL);
#endif
}
uint64_t nowms(void) {
    struct timeval tv;
    timeofday(&tv);
    return (uint64_t)tv.tv_usec / 1000 + (uint64_t)tv.tv_sec * 1000;
}
uint64_t nowsec(void) {
    struct timeval tv;
    timeofday(&tv);
    return (uint64_t)tv.tv_sec;
}
void nowtime(const char *fmt, char time[TIME_LENS]) {
    struct timeval tv;
    timeofday(&tv);
    time_t t = tv.tv_sec;
    strftime(time, TIME_LENS - 1, fmt, localtime(&t));
}
void nowmtime(const char *fmt, char time[TIME_LENS]) {
    struct timeval tv;
    timeofday(&tv);
    time_t t = tv.tv_sec;
    strftime(time, TIME_LENS - 1, fmt, localtime(&t));
    size_t uilen = strlen(time);
    SNPRINTF(time + uilen, TIME_LENS - uilen - 1, " %03d", (int32_t)(tv.tv_usec / 1000));
}
void fill_timespec(struct timespec* timeout, uint32_t ms) {
    if (ms >= 1000) {
        timeout->tv_sec = ms / 1000;
        timeout->tv_nsec = (long)(ms - timeout->tv_sec * 1000) * (1000 * 1000);
    } else {
        timeout->tv_sec = 0;
        timeout->tv_nsec = ms * (1000 * 1000);
    }
}
uint64_t hash(const char *buf, size_t len) {
    uint64_t rtn = 0;
    for (; len > 0; --len) {
        rtn = (rtn * 131) + *buf++;
    }
    return rtn;
}
void *memichr(const void *ptr, int32_t val, size_t maxlen) {
    char *buf = (char *)ptr;
    val = tolower(val);
    while (maxlen--) {
        if (tolower(*buf) == val) {
            return (void *)buf;
        }
        buf++;
    }
    return NULL;
}
#ifndef OS_WIN
int32_t _memicmp(const void *ptr1, const void *ptr2, size_t lens) {
    int32_t i = 0;
    char *buf1 = (char *)ptr1;
    char *buf2 = (char *)ptr2;
    while (tolower(*buf1) == tolower(*buf2)
        && i < (int32_t)lens) {
        buf1++;
        buf2++;
        i++;
    }
    if (i == (int32_t)lens) {
        return 0;
    } else {
        if (*buf1 > *buf2) {
            return 1;
        } else {
            return -1;
        }
    }
}
#endif
void *memstr(int32_t ncs, const void *ptr, size_t plens, const void *what, size_t wlen) {
    if (NULL == ptr
        || NULL == what
        || 0 == plens
        || 0 == wlen
        || wlen > plens) {
        return NULL;
    }
    chr_func chr;
    cmp_func cmp;
    if (0 == ncs) {
        chr = memchr;
        cmp = memcmp;
    } else {
        chr = memichr;
        cmp = _memicmp;
    }
    char *pos;
    char *wt = (char *)what;
    char *cur = (char *)ptr;
    do {
        pos = chr(cur, wt[0], plens - (size_t)(cur - (char*)ptr));
        if (NULL == pos
            || plens - (size_t)(pos - (char*)ptr) < wlen) {
            return NULL;
        }
        if (0 == cmp(pos, what, wlen)) {
            return (void *)pos;
        }
        cur = pos + 1;
    } while (plens - (size_t)(cur - (char*)ptr) >= wlen);
    return NULL;
}
void *skipempty(const void *ptr, size_t plens) {
    char *cur = (char *)ptr;
    while (' ' == *cur
        && (size_t)(cur - (char *)ptr) < plens) {
        cur++;
    }
    if (cur - (char *)ptr == plens) {
        return NULL;
    }
    return cur;
}
char *strupper(char *str){
    if (NULL == str) {
        return NULL;
    }
    char* p = str;
    while (*p != '\0') {
        if (*p >= 'a'
            && *p <= 'z') {
            *p &= ~0x20;
        }
        ++p;
    }
    return str;
}
char *strlower(char *str) {
    if (NULL == str) {
        return NULL;
    }
    char *p = str;
    while (*p != '\0') {
        if (*p >= 'A' && *p <= 'Z') {
            BIT_SET(*p, 0x20);
        }
        ++p;
    }
    return str;
}
char* strreverse(char* str) {
    if (NULL == str) {
        return NULL;
    }
    char* b = str;
    char* e = str;
    while (*e) { 
        ++e;
    }
    --e;
    char tmp;
    while (e > b) {
        tmp = *e;
        *e = *b;
        *b = tmp;
        --e;
        ++b;
    }
    return str;
}
int32_t randrange(int32_t min, int32_t max) {
    ASSERTAB(max > min, "rand range max must big than min.");
    return (rand() % (max - min)) + min;
}
char *randstr(char *buf, size_t len) {
    static char characters[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U',
        'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
        'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };
    size_t i = 0;
    for (; i < len; i++) {
        buf[i] = characters[randrange(0, sizeof(characters))];
    }
    buf[i] = '\0';
    return buf;
}
static char HEX[16] = {
    '0', '1', '2', '3',
    '4', '5', '6', '7',
    '8', '9', 'A', 'B',
    'C', 'D', 'E', 'F'
};
char *tohex(const unsigned char *buf, size_t len, char *out) {
    size_t j = 0;
    size_t i = 0;
    for (i = 0; i < len; ++i) {
        int t = buf[i];
        int a = t / 16;
        int b = t % 16;
        out[j] = HEX[a];
        ++j;
        out[j] = HEX[b];
        ++j;
    }
    out[j] = '\0';
    return out;
}
char *formatargs(const char *fmt, va_list args) {
    int32_t num;
    size_t size = 256;
    char *pbuff;
    CALLOC(pbuff, 1, size);    
    while (1) {
        num = vsnprintf(pbuff, size, fmt, args);
        if ((num > -1)
            && (num < (int32_t)size)) {
            return pbuff;
        }
        FREE(pbuff);
        //分配更大空间
        size = (num > -1) ? (num + 1) : size * 2;
        CALLOC(pbuff, 1, size);
    }
}
char *formatv(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *buf = formatargs(fmt, args);
    va_end(args);
    return buf;
}
