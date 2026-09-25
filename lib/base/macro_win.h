#ifndef MACRO_WIN_H_
#define MACRO_WIN_H_

#include "base/os.h"

#ifdef OS_WIN

#define IS_LITTLE 1 // 主机字节序：Windows 各目标都是小端
#define DLL_EXNAME "dll" // 动态库扩展名
#define PATH_SEPARATOR '\\' // 路径分隔符
#define PATH_SEPARATORSTR "\\" // 路径分隔符字符串
#define PATH_LENS 1024 // 路径最大长度；取 1024 只为长路径不被截断，超过 MAX_PATH(260) 的路径文件 API 仍打不开
#define INVALID_SOCK INVALID_SOCKET // 无效 socket 句柄

#define IS_EAGAIN(e) (WSAEWOULDBLOCK == (e) || EAGAIN == (e)) // 判断是否为非阻塞重试错误
#define GETPID   _getpid // 获取当前进程 ID
#define STRTOK   strtok_s // 线程安全的字符串分割
#define SNPRINTF snprintf // 格式化输出到缓冲区
#define SWPRINTF swprintf // 宽字符格式化输出
#define FSTAT    _stat // 获取文件状态
// 微秒级睡眠（Windows 使用可等待定时器实现）
#define USLEEP(us)\
    do {\
        uint64_t _slus = (uint64_t)(us);\
        LARGE_INTEGER _slft;\
        _slft.QuadPart = -(__int64)(_slus * 10ULL);\
        HANDLE _sltimer = CreateWaitableTimer(NULL, TRUE, NULL);\
        if (NULL != _sltimer) {\
            if (SetWaitableTimer(_sltimer, &_slft, 0, NULL, NULL, 0)) {\
                WaitForSingleObject(_sltimer, INFINITE);\
            }\
            CloseHandle(_sltimer);\
        }\
    }while(0)

#define MSLEEP(ms) Sleep(ms) // 毫秒级睡眠
#define THREAD_YIELD() SwitchToThread() // OS 级线程让出，用于自旋超限后的兜底退避
/* 自旋等待 CPU 暂停提示。CPU_PAUSE_CYCLES 的口径见 macro_unix.h 同名宏，
   两边必须一致。YieldProcessor 在 ARM 上展开成 yield、在 x86 上展开成 _mm_pause */
#define CPU_PAUSE() YieldProcessor()
#if defined(ARCH_ARM64) || defined(ARCH_ARM)
    #define CPU_PAUSE_CYCLES 1
#else
    #define CPU_PAUSE_CYCLES 140
#endif
#define THREAD_LOCAL __declspec(thread) // 线程局部存储
#define TIMEB  _timeb // 时间结构体类型
#define FTIME  _ftime // 获取当前时间（毫秒精度）
#define ACCESS _access // 检查文件访问权限
#define MKDIR  _mkdir // 创建目录
#define SHUT_RD   SD_RECEIVE // 关闭接收方向
#define SHUT_WR   SD_SEND // 关闭发送方向
#define SHUT_RDWR SD_BOTH // 关闭双向
#define SOCK_CLOSE closesocket // 关闭 socket
#define SET_CLOEXEC(fd) (void)SetHandleInformation((HANDLE)(fd), HANDLE_FLAG_INHERIT, 0) // 标记句柄不被子进程继承
// 关闭句柄并置空。漏掉置空就会被第二个收尾路径二次关闭(popen_close / popen_free 即成对)
#define CLOSE_HANDLE(h)\
    do {\
        if (NULL != (h)) {\
            CloseHandle((h));\
            (h) = NULL;\
        }\
    } while(0)
// 线程安全的本地时间转换；返回 0 成功、非 0 失败。调用方必须判返回值再用 dt
#define LOCALTIME(ts, dt) localtime_s((dt), (ts))
#define GMTIME(ts, dt) gmtime_s((dt), (ts))
#define ERRNO GetLastError() // 获取上一个 Windows 错误码
#define ERRORSTR(errcode) _fmterror(errcode) // 将错误码转换为字符串
// 将 Windows 错误码转换为可读字符串（内部使用 FormatMessageA）。定义在 base.c，全程序只一份线程局部缓冲；
// 返回那块缓冲（FormatMessageA 失败时返回固定串），不用释放，本线程下次调用前有效
const char *_fmterror(DWORD error);

#endif
#endif//MACRO_WIN_H_
