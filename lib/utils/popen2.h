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
    pid_t pid;
    int sock;
#endif
}popen_ctx;
//mode  rw
int32_t popen_startup(popen_ctx *ctx, const char *cmd, const char *mode);
void popen_close(popen_ctx *ctx);
void popen_free(popen_ctx *ctx);
int32_t popen_waitexit(popen_ctx *ctx, uint32_t ms);
//非windows 不一定能取到
int32_t popen_exitcode(popen_ctx *ctx);
int32_t popen_read(popen_ctx *ctx, char *output, size_t lens);
//input \n结束 才会执行
int32_t popen_write(popen_ctx *ctx, const char *input, size_t lens);

#endif//POPEN2_H_
