#ifndef MACRO_WIN_H_
#define MACRO_WIN_H_

#include "base/os.h"

#ifdef OS_WIN

#define DLL_EXNAME "dll"           // 动态库扩展名
#define PATH_SEPARATOR '\\'        // 路径分隔符
#define PATH_SEPARATORSTR "\\"     // 路径分隔符字符串
#define PATH_LENS MAX_PATH         // 路径最大长度
#define INVALID_SOCK INVALID_SOCKET // 无效 socket 句柄
#define IS_EAGAIN(e) (WSAEWOULDBLOCK == (e) || EAGAIN == (e)) // 判断是否为非阻塞重试错误
#define GETPID   _getpid           // 获取当前进程 ID
#define STRICMP  _stricmp          // 不区分大小写字符串比较
#define STRNCMP  _strnicmp         // 不区分大小写的前 n 字节字符串比较
#define STRTOK   strtok_s          // 线程安全的字符串分割
#define SNPRINTF _snprintf         // 格式化输出到缓冲区
#define SWPRINTF swprintf          // 宽字符格式化输出
#define STRNCPY  strncpy_s         // 安全的固定长度字符串复制
#define FSTAT    _stat             // 获取文件状态
// 微秒级睡眠（Windows 使用可等待定时器实现）
#define USLEEP(us)\
    do {\
        LARGE_INTEGER ft;\
        ft.QuadPart = -(10 * (__int64)us);\
        HANDLE timer = CreateWaitableTimer(NULL, TRUE, NULL);\
        if (NULL != timer) {\
            if (SetWaitableTimer(timer, &ft, 0, NULL, NULL, 0)) {\
                WaitForSingleObject(timer, INFINITE);\
            }\
            CloseHandle(timer);\
        }\
    }while(0)
#define MSLEEP(ms) Sleep(ms)       // 毫秒级睡眠
// 自旋等待 CPU 暂停提示（Windows）
#define CPU_PAUSE() YieldProcessor()
// OS 级线程让出，用于自旋超限后的兜底退避
#define THREAD_YIELD() SwitchToThread()
#define TIMEB  _timeb              // 时间结构体类型
#define FTIME  _ftime              // 获取当前时间（毫秒精度）
#define ACCESS _access             // 检查文件访问权限
#define MKDIR  _mkdir              // 创建目录
#define SHUT_RD   SD_RECEIVE       // 关闭接收方向
#define SHUT_WR   SD_SEND          // 关闭发送方向
#define SHUT_RDWR SD_BOTH          // 关闭双向
#define SOCK_CLOSE closesocket     // 关闭 socket
#define ERRNO GetLastError()       // 获取上一个 Windows 错误码
// 将 Windows 错误码转换为可读字符串（内部使用 FormatMessageA）
static inline const char *_fmterror(DWORD error) {
    char *err = NULL;
    if (0 == FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL,
                            error,
                            MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                            (LPTSTR)&err,
                            0,
                            NULL)) {
        return "FormatMessageA error.";
    }
    static __declspec(thread) char errstr[4096]; // 线程局部存储，避免多线程竞争
    size_t ilens = strlen(err);
    ilens = ilens >= sizeof(errstr) ? sizeof(errstr) - 1 : ilens;
    memcpy(errstr, err, ilens);
    errstr[ilens] = '\0';
    LocalFree(err);
    return errstr;
};
#define ERRORSTR(errcode) _fmterror(errcode) // 将错误码转换为字符串

typedef uint32_t atomic_t;   // 32 位原子整数类型（Windows）
typedef uint64_t atomic64_t; // 64 位原子整数类型（Windows）
// InterlockedExchangeAdd：原子加 32 位，返回旧值
#define ATOMIC_ADD(ptr, val) InterlockedExchangeAdd(ptr, val)
// InterlockedExchange：原子交换 32 位，返回旧值
#define ATOMIC_SET(ptr, val) InterlockedExchange(ptr, val)
// InterlockedCompareExchange：若 *ptr == oldval 则设为 newval，返回旧值；成功时返回值等于 oldval
#define ATOMIC_CAS(ptr, oldval, newval) (InterlockedCompareExchange(ptr, newval, oldval) == oldval)
// 64 位原子操作（与 32 位对应）
#define ATOMIC64_ADD(ptr, val) InterlockedExchangeAdd64(ptr, val)
#define ATOMIC64_SET(ptr, val) InterlockedExchange64(ptr, val)
#define ATOMIC64_CAS(ptr, oldval, newval) (InterlockedCompareExchange64(ptr, newval, oldval) == oldval)

#endif
#endif//MACRO_WIN_H_
