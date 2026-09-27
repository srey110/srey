#ifndef POPEN2_H_
#define POPEN2_H_

#include "base/macro.h"

typedef struct popen_ctx {
    int32_t closed;   //popen_close 是否被调用过；popen_free 据此决定要不要兜底收尾
#ifdef OS_WIN
    HANDLE pipe[2];              //命名管道句柄对：[0] 服务端（子进程端），[1] 客户端（父进程端）
    PROCESS_INFORMATION process; //子进程信息
    HANDLE job;                  //子进程所在的作业对象，popen_close 靠它连同孙进程一起结束；NULL 表示没建成，只能结束子进程本身。
                                 //子进程挂起创建、放进作业后才放行，否则它抢先派生的孙进程不在作业里
#else
    int32_t exited;   //子进程是否已退出
    int32_t exitcode; //子进程退出码
    SOCKET sock;      //与子进程通信的套接字（父进程端）
    pid_t pid;        //子进程 PID
#endif
}popen_ctx;
/// <summary>
/// 执行命令
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="cmd">命令</param>
/// <param name="mode">r读 w写</param>
/// <returns>ERR_OK 成功</returns>
int32_t popen_startup(popen_ctx *ctx, const char *cmd, const char *mode);
/// <summary>
/// 关闭进程：杀掉整个子进程组（含 shell 派生的孙进程）并收尸。重复调用安全。
/// 子进程已自行退出时不做任何事，只置内部标记
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
/// 期间阻塞调用线程
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="ms">超时 毫秒；0 只探一次不等</param>
/// <returns>ERR_OK 已退出(退出码已记下，见 popen_exitcode)或本来就没有子进程；超时或 waitpid 出错 ERR_FAILED</returns>
int32_t popen_waitexit(popen_ctx *ctx, uint32_t ms);
/// <summary>
/// 获取退出码 非windows 不一定能取到；须在 popen_free 之前调用（free 后 windows 已不持有进程句柄）
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <returns>退出码</returns>
int32_t popen_exitcode(popen_ctx *ctx);
/// <summary>
/// 非阻塞单次探测读取子进程输出：有数据则读当前可读的一批（最多 lens 字节）并返回；无数据则立即返回不阻塞。
/// 通过 eof 出参区分“子进程暂无输出”与“已到流末尾”，避免把暂无输出误判为 EOF
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="output">输出</param>
/// <param name="lens">长度</param>
/// <param name="eof">出参，可为 NULL：置 1=已到流末尾（写端全关、不再有输出），置 0=未到（读到数据/暂无数据/出错）</param>
/// <returns>读到的字节数；0 表示当前无数据（配合 eof 区分暂无输出与 EOF）；ERR_FAILED 失败</returns>
int32_t popen_read(popen_ctx *ctx, char *output, size_t lens, int32_t *eof);
/// <summary>
/// 写入,\n结束 才会执行 w
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="input">输入</param>
/// <param name="lens">长度</param>
/// <returns>写入的字节数，可能少于 lens——写端非阻塞，对端缓冲写满即返回，
/// 剩余部分由调用方决定重试还是放弃；一个字节都没写进去且出错时返回 ERR_FAILED</returns>
int32_t popen_write(popen_ctx *ctx, const char *input, size_t lens);

#endif//POPEN2_H_
