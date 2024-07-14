#ifndef POPEN2_H_
#define POPEN2_H_

#include "base/macro.h"

typedef struct popen_ctx {
#ifdef OS_WIN
    HANDLE pipe[2];
    PROCESS_INFORMATION process;
#else
    int32_t exited;
    int32_t exitcode;
    SOCKET sock;
    pid_t pid;
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
/// 释放
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
/// 获取退出码 非windows 不一定能取到
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <returns>退出码</returns>
int32_t popen_exitcode(popen_ctx *ctx);
/// <summary>
/// 获取命令的输出 r
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="output">输出</param>
/// <param name="lens">长度</param>
/// <returns>读到的字节数, ERR_FAILED 失败</returns>
int32_t popen_read(popen_ctx *ctx, char *output, size_t lens);
/// <summary>
/// 向命令写入,\n结束 才会执行 w
/// </summary>
/// <param name="ctx">popen_ctx</param>
/// <param name="input">输入</param>
/// <param name="lens">长度</param>
/// <returns>写入的字节数, ERR_FAILED 失败</returns>
int32_t popen_write(popen_ctx *ctx, const char *input, size_t lens);

#endif//POPEN2_H_
