#ifndef SYS_H_
#define SYS_H_

#include "base/os.h"

/// <summary>
/// 当前线程ID
/// </summary>
/// <returns>线程ID</returns>
uint64_t threadid(void);
/// <summary>
/// 放开 core 文件大小与打开文件数：只把软上限抬到硬上限以内(文件数要 65535，macOS 为 OPEN_MAX)，已够就不动，硬上限不改，
/// 文件数被硬上限压在目标以下时打 WARN；Windows 上注册崩溃时写 MiniDump，其余平台预先调一次 backtrace(理由见实现)
/// </summary>
void unlimit(void);
/// <summary>
/// 信号处理。cb 在信号处理上下文中被调用，内部只能用 async-signal-safe 函数
/// （LOG_INFO / mutex_lock / malloc 都不行）；安全做法是 cb 里只置
/// volatile sig_atomic_t 标志，主循环轮询后再处理。
/// </summary>
/// <param name="cb">处理函数（必须 async-signal-safe）</param>
/// <param name="data">参数</param>
void sighandle(void(*cb)(int32_t, void *), void *data);
/// <summary>
/// cpu核心数。首次取到后缓存，运行期 CPU 热插拔不再反映；各线程同时首调只是各算一遍同一个值
/// </summary>
/// <returns>核心数</returns>
uint32_t procscnt(void);
/// <summary>
/// 本进程可用的 CPU 核数：Linux 取 CPU 亲和(taskset / cpuset)与 cgroup v2 cpu.max 配额(向上取整)的较小值，
/// FreeBSD 取 CPU 亲和，其余平台同 procscnt。亲和取的是首次调用所在线程的，先把这个线程钉核再调会算少；
/// 缓存口径同 procscnt，运行期改亲和或配额也不再反映
/// </summary>
/// <returns>可用核数，至少 1</returns>
uint32_t procscnt_avail(void);
// 把 cgroup v2 cpu.max 的内容("配额 周期"，如 "150000 100000"，'\0' 结尾)折成核数，向上取整；
// "max"(不限)、配额或周期为 0、解析不了时返回 0 表示不限。procscnt_avail 内部用，放出来给测试
uint32_t _cgroup_cpu_quota(const char *text);
/// <summary>
/// 当前程序所在路径
/// </summary>
/// <returns>路径</returns>
const char *procpath(void);

#endif//SYS_H_
