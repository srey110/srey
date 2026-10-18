#ifndef POPEN2_H_
#define POPEN2_H_

#include "base/base.h"

typedef struct popen_ctx {
    int32_t closed;   //popen_close 是否被调用过；popen_free 据此决定要不要兜底收尾
#ifdef OS_WIN
    HANDLE pipe[2];              //父进程端的管道：[0] 读子进程输出（stdout 与 stderr），[1] 写子进程 stdin；NULL 表示没接管或已关
    PROCESS_INFORMATION process; //子进程信息
    HANDLE job;                  //子进程所在的作业对象，popen_close 靠它连同孙进程一起结束；NULL 表示没建成，只能结束子进程本身。
                                 //子进程挂起创建、放进作业后才放行，否则它抢先派生的孙进程不在作业里
#else
    int32_t exited;   //子进程已退出、退出码已记下；收尸只在 popen_close 里做
    int32_t exitcode; //子进程退出码
    int32_t wopen;    //sock 还接着子进程 stdin：mode 含 'w' 且没调过 popen_close_stdin
    int32_t ropen;    //sock 接着子进程输出：mode 含 'r'
    SOCKET sock;      //与子进程通信的套接字（父进程端）
    pid_t pid;        //子进程 PID
#endif
}popen_ctx;

/// <summary>
/// 起子进程执行命令。POSIX 经 /bin/sh -c 执行（支持管道、重定向与 shell 内建命令）；
/// Windows 直接交给 CreateProcess、不经 cmd.exe，要管道或内建命令须自己写成 "cmd /c ..."。
/// 子进程自成进程组（Windows 放进作业对象）；POSIX 上 SIGPIPE 恢复默认处置，
/// 不接管的 stdin 接 /dev/null（Windows 沿用本进程的），不接管的 stdout / stderr 沿用本进程的。
/// Windows 上子进程只继承交给它的标准流句柄，本进程别的句柄一个也不带过去
/// </summary>
/// <param name="ctx">popen_ctx，函数内先整体清零；失败后仍可安全调 popen_free</param>
/// <param name="cmd">命令</param>
/// <param name="mode">含 'r' 读子进程输出（stdout 与 stderr 并进同一通道），含 'w' 写子进程 stdin，"rw" 两者都要；
/// NULL 或两者都不含表示不接管道</param>
/// <returns>ERR_OK 成功；ERR_FAILED cmd 为 NULL 或空串、建管道或起进程失败</returns>
int32_t popen_startup(popen_ctx *ctx, const char *cmd, const char *mode);
/// <summary>
/// 关闭进程：结束整个子进程组，连同 shell 派生的孙进程（Windows 为整个作业）；
/// 直接子进程已自行退出时也照样清理组里剩下的进程。重复调用安全。
/// POSIX 同步收尸后返回（只在这里收尸）；Windows 只发起终止、不等它结束，要取退出码先 popen_waitexit
/// </summary>
/// <param name="ctx">popen_ctx</param>
void popen_close(popen_ctx *ctx);
/// <summary>
/// 释放；关闭后句柄置空，重复调用安全，此后 read/write 返回失败。
/// 没调用过 popen_close 的话本函数替你调一次——free 一执行调用方就永久失去了子进程句柄，
/// 不收尾就是永久孤儿（子进程自成进程组，终端信号也够不到它）
/// </summary>
/// <param name="ctx">popen_ctx</param>
void popen_free(popen_ctx *ctx);
/// <summary>
/// 等待执行完成：子进程一退出就返回(Linux 等 pidfd、kqueue 平台等 NOTE_EXIT，都用不了时退避轮询)，
/// 期间阻塞调用线程；超时按单调时钟计。
/// 'r' 模式下输出可能超过管道缓冲时要边读边等：先等退出再读，子进程会卡在写上退不了，只能等满超时。
/// POSIX 上只记下退出码、不收尸：收尸前进程组号不会被别的进程复用，popen_close 才能放心清理整组
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="ms">超时 毫秒；0 只探一次不等</param>
/// <returns>ERR_OK 已退出(退出码已记下，见 popen_exitcode)或本来就没有子进程；超时或 waitid 出错 ERR_FAILED</returns>
int32_t popen_waitexit(popen_ctx *ctx, uint32_t ms);
/// <summary>
/// 获取退出码。须在 popen_waitexit 返回 ERR_OK 或 popen_close 之后、popen_free 之前调用
/// （free 后 Windows 已不持有进程句柄）
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <returns>子进程的退出码；被信号终止、被 popen_close 结束、还没退出、没有子进程或出错时 ERR_FAILED</returns>
int32_t popen_exitcode(popen_ctx *ctx);
/// <summary>
/// 非阻塞单次探测读取子进程输出：有数据则读当前可读的一批（最多 lens 字节）并返回；无数据则立即返回不阻塞。
/// 通过 eof 出参区分“子进程暂无输出”与“已到流末尾”，避免把暂无输出误判为 EOF。
/// 要拿全输出就反复读到 eof 为 1，与等退出交替进行（见 popen_waitexit）
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="output">输出</param>
/// <param name="lens">长度</param>
/// <param name="eof">出参，可为 NULL：置 1=已到流末尾（写端全关、不再有输出），置 0=未到（读到数据/暂无数据/出错）</param>
/// <returns>读到的字节数；0 表示当前无数据（配合 eof 区分暂无输出与 EOF）；mode 不含 'r'、已 popen_free 或读出错时 ERR_FAILED</returns>
int32_t popen_read(popen_ctx *ctx, char *output, size_t lens, int32_t *eof);
/// <summary>
/// 写入子进程 stdin，当场写进管道；子进程按行读的话换行要调用方自己带
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="input">输入</param>
/// <param name="lens">长度；超过 INT32_MAX 时本次最多写 INT32_MAX 字节</param>
/// <returns>写入的字节数，可能少于 lens——写端非阻塞，对端缓冲写满即返回，单次也不超过 INT32_MAX，
/// 剩余部分由调用方决定重试还是放弃；mode 不含 'w'、已 popen_close_stdin，或一个字节都没写进去且出错时返回 ERR_FAILED</returns>
int32_t popen_write(popen_ctx *ctx, const char *input, size_t lens);
/// <summary>
/// 只关子进程的 stdin、让它读到 EOF，子进程照常运行、输出照常可读；
/// 读 stdin 到 EOF 才出结果的程序（sort、wc 之类）靠它收尾。之后 popen_write 一律失败
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <returns>ERR_OK 已送出 EOF；ERR_FAILED mode 不含 'w'、已调过本函数、已 popen_free 或关闭出错</returns>
int32_t popen_close_stdin(popen_ctx *ctx);

#endif//POPEN2_H_
