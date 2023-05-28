#ifndef BUFFER_H_
#define BUFFER_H_

#include "macro.h"

#if defined(OS_WIN)
#define IOV_TYPE WSABUF
#define IOV_PTR_FIELD buf
#define IOV_LEN_FIELD len
#define IOV_LEN_TYPE ULONG
#else
//struct iovec {
//    void *iov_base;
//    size_t iov_len;
//};
#define IOV_TYPE struct iovec
#define IOV_PTR_FIELD iov_base
#define IOV_LEN_FIELD iov_len
#define IOV_LEN_TYPE size_t
#endif
#define MAX_EXPAND_NIOV          4
typedef struct buffer_ctx {
    volatile int32_t freeze_read;
    volatile int32_t freeze_write;
    struct bufnode_ctx *head;
    struct bufnode_ctx *tail;
    struct bufnode_ctx **tail_with_data;
    size_t total_len;//数据总长度
}buffer_ctx;

void buffer_init(buffer_ctx *ctx);
void buffer_free(buffer_ctx *ctx);

size_t buffer_size(buffer_ctx *ctx);
int32_t buffer_append(buffer_ctx *ctx, void *data, const size_t len);
int32_t buffer_appendv(buffer_ctx *ctx, const char *fmt, ...);
int32_t buffer_copyout(buffer_ctx *ctx, const size_t start, void *out, size_t len);
int32_t buffer_drain(buffer_ctx *ctx, size_t len);
int32_t buffer_remove(buffer_ctx *ctx, void *out, size_t len);
//ncs 1 不区分大小写
int32_t buffer_search(buffer_ctx *ctx, const int32_t ncs,
    const size_t start, const size_t end, char *what, size_t wlen);

uint32_t buffer_expand(buffer_ctx *ctx, const size_t lens, IOV_TYPE *iov, const uint32_t cnt);
void buffer_commit_expand(buffer_ctx *ctx, size_t len ,IOV_TYPE *iov, const uint32_t cnt);

uint32_t buffer_get(buffer_ctx *ctx, size_t atmost, IOV_TYPE *iov, const uint32_t cnt);
void buffer_commit_get(buffer_ctx *ctx, size_t size);

int32_t buffer_from_sock(buffer_ctx *ctx, SOCKET fd, size_t *nread,
    int32_t(*_readv)(SOCKET, IOV_TYPE *, uint32_t, void *), void *arg);

#endif//BUFFER_H_
