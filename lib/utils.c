#include "utils.h"
#include "md5/md5.h"
#include "sha1/sha1.h"

#ifdef OS_WIN
#pragma warning(disable:4091)
#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib" )
static atomic_t _exindex = 0;
#endif

static void *_ud;
static void(*_sig_cb)(int32_t, void *);
static atomic64_t _ids = 1000;
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
static void _ResetPrivilege(HANDLE handle, TOKEN_PRIVILEGES *privold) {
    (void)AdjustTokenPrivileges(handle, FALSE, privold, 0, NULL, NULL);
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
        _ResetPrivilege(ptoken, &tprivold);
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
    signal(SIGINT, _sighandler);//终止进程
    signal(SIGHUP, _sighandler);//终止进程
    signal(SIGTSTP, _sighandler);//ctrl+Z
    signal(SIGTERM, _sighandler);//终止一个进程
    signal(SIGKILL, _sighandler);//立即结束程序
    signal(SIGABRT, _sighandler);//中止一个程序
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
static uint16_t crc16_tab[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040
};
uint16_t crc16(const char *buf, const size_t len) {
    uint16_t uscrc = 0;
    const uint8_t *tmp = (const uint8_t *)buf;
    for (size_t i = 0; i < len; ++i) {
        uscrc = (uscrc >> 8) ^ crc16_tab[(uscrc ^ *tmp++) & 0xFF];
    }
    return uscrc;
}
static uint32_t crc32_tab[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba,
    0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
    0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
    0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdbbbc9d6, 0xacbcf940,
    0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116,
    0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f,
    0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a,
    0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
    0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c,
    0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
    0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086,
    0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4,
    0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
    0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe,
    0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252,
    0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60,
    0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04,
    0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
    0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e,
    0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
    0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
    0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6,
    0xbad03605, 0xcdd70693, 0x54de5729, 0x23d967bf,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};
uint32_t crc32(const char *buf, const size_t len) {
    uint32_t uicrc = ~0U;
    const uint8_t *tmp = (const uint8_t *)buf;
    for (size_t i = 0; i < len; ++i) {
        uicrc = crc32_tab[(uicrc ^ *tmp++) & 0xFF] ^ (uicrc >> 8);
    }
    return uicrc ^ ~0U;
}
void md5(const char *buf, const size_t len, char md5str[33]) {
    md5_byte_t digest[16];
    md5_state_t stmd5;
    md5_init(&stmd5);
    md5_append(&stmd5, (md5_byte_t*)buf, (int)len);
    md5_finish(&stmd5, digest);
    md5_tostring(digest, md5str);
}
void sha1(const char *buf, const size_t len, char sha1str[20]) {
    SHA1_CTX ctx;
    SHA1Init(&ctx);
    SHA1Update(&ctx, (const uint8_t *)buf, (uint32_t)len);
    SHA1Final((uint8_t *)sha1str, &ctx);
}
/* BASE 64 encode table */
static char base64en[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3',
    '4', '5', '6', '7', '8', '9', '+', '/',
};
#define BASE64_PAD      '='
#define BASE64DE_FIRST  '+'
#define BASE64DE_LAST   'z'
/* ASCII order for BASE 64 decode, -1 in unused character */
static char base64de[] = {
    62,  -1,  -1,  -1,  63,  52,  53,  54,
    55,  56,  57,  58,  59,  60,  61,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,   0,   1,
    2,   3,   4,   5,   6,   7,   8,   9,
    10,  11,  12,  13,  14,  15,  16,  17,
    18,  19,  20,  21,  22,  23,  24,  25,
    -1,  -1,  -1,  -1,  -1,  -1,  26,  27,
    28,  29,  30,  31,  32,  33,  34,  35,
    36,  37,  38,  39,  40,  41,  42,  43,
    44,  45,  46,  47,  48,  49,  50,  51,
};
#define B64_ENSIZE(s)   (((s) + 2) / 3 * 4)
char *b64encode(const char *buf, const size_t len, size_t *new_len) {
    int32_t s;
    uint32_t i, j;
    char *out;
    size_t enlen = B64_ENSIZE(len) + 1;
    CALLOC(out, 1, enlen);
    for (i = j = 0; i < len; i++) {
        s = i % 3;
        switch (s) {
        case 0:
            out[j++] = base64en[(buf[i] >> 2) & 0x3F];
            continue;
        case 1:
            out[j++] = base64en[((buf[i - 1] & 0x3) << 4) + ((buf[i] >> 4) & 0xF)];
            continue;
        case 2:
            out[j++] = base64en[((buf[i - 1] & 0xF) << 2) + ((buf[i] >> 6) & 0x3)];
            out[j++] = base64en[buf[i] & 0x3F];
        }
    }
    /* move back */
    i -= 1;
    /* check the last and add padding */
    switch (i % 3) {
    case 0:
        out[j++] = base64en[(buf[i] & 0x3) << 4];
        out[j++] = BASE64_PAD;
        out[j++] = BASE64_PAD;
        break;
    case 1:
        out[j++] = base64en[(buf[i] & 0xF) << 2];
        out[j++] = BASE64_PAD;
        break;
    }
    *new_len = j;
    return out;
}
#define B64_DESIZE(s)   (((s)) / 4 * 3)
char *b64decode(const char *buf, const size_t len, size_t *new_len) {
    int32_t c, s;
    uint32_t i, j;
    char *out;
    size_t delen = B64_DESIZE(len) + 1;
    CALLOC(out, 1, delen);
    for (i = j = 0; i < len; i++) {
        s = i % 4;
        if (buf[i] == '=') {
            *new_len = j;
            return out;
        }
        if (buf[i] < BASE64DE_FIRST
            || buf[i] > BASE64DE_LAST
            || (c = base64de[buf[i] - BASE64DE_FIRST]) == -1) {
            FREE(out);
            return NULL;
        }
        switch (s) {
        case 0:
            out[j] = ((uint32_t)c << 2) & 0xFF;
            continue;
        case 1:
            out[j++] += ((uint32_t)c >> 4) & 0x3;
            if (i < (len - 3) || buf[len - 2] != '=') {
                out[j] = ((uint32_t)c & 0xF) << 4;
            }
            continue;
        case 2:
            out[j++] += ((uint32_t)c >> 2) & 0xF;
            if (i < (len - 2) || buf[len - 1] != '=') {
                out[j] = ((uint32_t)c & 0x3) << 6;
            }
            continue;
        case 3:
            out[j++] += (uint8_t)c;
        }
    }
    *new_len = j;
    return out;
}
char *xorencode(const char key[4], const size_t round, char *buf, const size_t len) {
    for (size_t i = 0; i < round; i++) {
        buf[0] = ((buf[0] + key[1]) ^ key[2]) ^ key[3];
        for (size_t j = 1; j < len; j++) {
            buf[j] = (buf[j - 1] + buf[j]) ^ key[0];
        }
    }
    return buf;
}
char *xordecode(const char key[4], const size_t round, char *buf, const size_t len) {
    for (size_t i = 0; i < round; i++) {
        for (size_t j = len - 1; j > 0; j--) {
            buf[j] = (buf[j] ^ key[0]) - buf[j - 1];
        }
        buf[0] = ((buf[0] ^ key[3]) ^ key[2]) - key[1];
    }
    return buf;
}
static unsigned char hexchars[] = "0123456789ABCDEF";
char *urlencode(const char *str, const size_t len, size_t *new_len) {
    register unsigned char c;
    unsigned char *to, *start;
    unsigned char const *from, *end;
    from = (unsigned char *)str;
    end = (unsigned char *)str + len;
    CALLOC(start, (size_t)1, (size_t)(3 * len + 1));
    to = start;
    while (from < end) {
        c = *from++;
        if (c == ' ') {
            *to++ = '+';
        } else if ((c < '0' && c != '-' && c != '.') ||
            (c < 'A' && c > '9') ||
            (c > 'Z' && c < 'a' && c != '_') ||
            (c > 'z')) {
            to[0] = '%';
            to[1] = hexchars[c >> 4];
            to[2] = hexchars[c & 15];
            to += 3;
        } else {
            *to++ = c;
        }
    }
    *to = 0;
    if (new_len) {
        *new_len = (int32_t)(to - start);
    }
    return (char *)start;
}
static int32_t _htoi(char *s) {
    int32_t c, value;
    c = ((unsigned char *)s)[0];
    if (isupper(c)) {
        c = tolower(c);
    }
    value = (c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10) * 16;
    c = ((unsigned char *)s)[1];
    if (isupper(c)) {
        c = tolower(c);
    }
    value += c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
    return (value);
}
int32_t urldecode(char *str, size_t len) {
    char *dest = str;
    char *data = str;
    while (len--) {
        if (*data == '+') {
            *dest = ' ';
        } else if (*data == '%'
            && len >= 2
            && isxdigit((int) *(data + 1))
            && isxdigit((int) *(data + 2))) {
            *dest = (char)_htoi(data + 1);
            data += 2;
            len -= 2;
        } else {
            *dest = *data;
        }
        data++;
        dest++;
    }
    *dest = '\0';
    return (int32_t)(dest - str);
}
uint64_t hash(const char *buf, size_t len) {
    uint64_t rtn = 0;
    for (; len > 0; --len) {
        rtn = (rtn * 131) + *buf++;
    }
    return rtn;
}
uint64_t fnv1a_hash(const char *buf, size_t len) {
#if defined(ARCH_X64)
#define OFFSET_BASIS 14695981039346656037ULL
#define PRIME 1099511628211ULL
#else
#define OFFSET_BASIS 2166136261UL
#define PRIME 16777619UL
#endif
    uint64_t rtn = OFFSET_BASIS;
    for (; len > 0; --len) {
        rtn ^= (uint64_t)*buf++;
        rtn *= PRIME;
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
    srand((unsigned int)time(NULL));
    for (; i < len; i++) {
        buf[i] = characters[randrange(0, sizeof(characters))];
    }
    buf[i] = '\0';
    return buf;
}
char *tohex(const char *buf, size_t len, char *out, size_t outlen) {
    size_t offset = 0;
    for (size_t i = 0; i < len; i++) {
        if (i == len - 1) {
            SNPRINTF(out + offset, outlen - offset - 1, "%02X", (uint8_t)buf[i]);
        } else {
            SNPRINTF(out + offset, outlen - offset - 1, "%02X ", (uint8_t)buf[i]);
        }        
        offset += 3;
    }
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
