#include "utils/hug.h"
#include "utils/utils.h"

int32_t hug_init(hug_ctx *ctx) {
    ATOMIC_SET(&ctx->exitflag, 0);
#ifdef OS_WIN
    mutex_init(&ctx->muexit);
    cond_init(&ctx->condexit);
#else
    // 先置 -1 兜底: pipe 失败时 hug_free 内 if (-1 != exit_pipe[0]) 不会 close 栈垃圾值误关其他 fd
    ctx->exit_pipe[0] = -1;
    ctx->exit_pipe[1] = -1;
    if (-1 == pipe(ctx->exit_pipe)) {
        PRINT("hug pipe failed: %s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    fcntl(ctx->exit_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(ctx->exit_pipe[1], F_SETFD, FD_CLOEXEC);
#endif
    return ERR_OK;
}
void hug_wait(hug_ctx *ctx) {
#ifdef OS_WIN
    mutex_lock(&ctx->muexit);
    while (0 == ATOMIC_GET(&ctx->exitflag)) {
        cond_wait(&ctx->condexit, &ctx->muexit);
    }
    mutex_unlock(&ctx->muexit);
#else
    char buf[16];
    while (0 == ATOMIC_GET(&ctx->exitflag)) {
        // 忽略返回值: 被信号打断 EINTR 或正常读到字节都重新 check exitflag
        (void)!read(ctx->exit_pipe[0], buf, sizeof(buf));
    }
#endif
}
void hug_wakeup(hug_ctx *ctx) {
#ifdef OS_WIN
    // waker 必须在 mutex 内 SET, 否则与 waiter mutex_lock->check flag->cond_wait
    // 之间存在 lost wakeup window (waker 在 waiter 进 wait queue 之前 signal 即丢失)
    mutex_lock(&ctx->muexit);
    ATOMIC_SET(&ctx->exitflag, 1);
    mutex_unlock(&ctx->muexit);
    cond_signal(&ctx->condexit);
#else
    // POSIX self-pipe: 写入字节驻留 kernel pipe buffer 不会丢失, 无需 mutex
    ATOMIC_SET(&ctx->exitflag, 1);
    char x = 1;
    (void)!write(ctx->exit_pipe[1], &x, 1);
#endif
}
void hug_free(hug_ctx *ctx) {
#ifdef OS_WIN
    mutex_free(&ctx->muexit);
    cond_free(&ctx->condexit);
#else
    if (-1 != ctx->exit_pipe[0]) {
        close(ctx->exit_pipe[0]);
        ctx->exit_pipe[0] = -1;
    }
    if (-1 != ctx->exit_pipe[1]) {
        close(ctx->exit_pipe[1]);
        ctx->exit_pipe[1] = -1;
    }
#endif
}
