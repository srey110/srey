#ifndef DYNALIB_H_
#define DYNALIB_H_

#include "base/macro.h"

typedef struct dl_ctx {
#ifdef OS_WIN
    HMODULE handle;
#else
#ifdef OS_HPUX
    shl_t handle;
#else
    void *handle;
#endif
#endif
}dl_ctx;

static inline int32_t dl_init(dl_ctx *ctx, const char *lib) {
#ifdef OS_WIN
    ctx->handle = LoadLibrary(lib);
#else
#ifdef OS_HPUX
    ctx->handle = shl_load(lib, BIND_DEFERRED | DYNAMIC_PATH, 0);
#else
    ctx->handle = dlopen(lib, RTLD_LAZY);
#endif
#endif
    return NULL == ctx->handle ? ERR_FAILED : ERR_OK;
}
static inline void dl_free(dl_ctx *ctx) {
    if (NULL == ctx->handle) {
        return;
    }
#ifdef OS_WIN
    FreeLibrary(ctx->handle);
#else
#ifdef OS_HPUX
    shl_unload(ctx->handle);
#else
    dlclose(ctx->handle);
#endif
#endif
}
static inline void *dl_sym(dl_ctx *ctx, const char *sym) {
#ifdef OS_WIN
    return GetProcAddress(ctx->handle, sym);
#else
#ifdef OS_HPUX
    void *func = NULL;
    shl_findsym(ctx->handle, sym, TYPE_PROCEDURE, &func);
    return func;
#else
    return dlsym(ctx->handle, sym);
#endif
#endif
}

#endif//DYNALIB_H_
