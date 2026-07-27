#ifndef POPEN2_H_
#define POPEN2_H_

#include "base/macro.h"

typedef struct popen_ctx {
#ifdef OS_WIN
    HANDLE pipe[2];              //命名管道句柄对：[0] 服务端（子进程端），[1] 客户端（父进程端）
    PROCESS_INFORMATION process; //子进程信息
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
/// 关闭进程
/// </summary>
/// <param name="ctx">popen_ctx</param>
void popen_close(popen_ctx *ctx);
/// <summary>
/// 释放；关闭后句柄置空，重复调用安全，此后 read/write 返回失败
/// </summary>
/// <param name="ctx">popen_ctx</param>
void popen_free(popen_ctx *ctx);
/// <summary>
/// 等待执行完成
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="ms">超时 毫秒</param>
/// <returns>ERR_OK 成功</returns>
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
/// <returns>写入的字节数, ERR_FAILED 失败</returns>
int32_t popen_write(popen_ctx *ctx, const char *input, size_t lens);

#endif//POPEN2_H_
