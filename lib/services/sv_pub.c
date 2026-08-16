#include "services/sv_pub.h"
#include "srey/coro.h"
#include "srey/loader.h"

int32_t _svpub_call_dst(task_ctx *dst, task_ctx *task, subtype_t req, void *buf, size_t lens) {
    int32_t erro = 0;
    size_t rsize = 0;
    coro_request(dst, task, req, buf, lens, 0, &erro, &rsize);
    task_ungrab(dst);
    return erro;
}
int32_t _svpub_send_dst(task_ctx *dst, task_ctx *task, subtype_t req, uint64_t sess,
                        void *buf, size_t lens) {
    if (0 == sess) {
        task_call(dst, req, buf, lens, 0);
    } else {
        task_request(dst, task, req, sess, buf, lens, 0);
    }
    task_ungrab(dst);
    return ERR_OK;
}
int32_t _svpub_call(task_ctx *task, name_t name, subtype_t req, void *buf, size_t lens) {
    task_ctx *dst = task_grab(task->loader, name);
    if (NULL == dst) {
        FREE(buf);
        return ERR_FAILED;
    }
    return _svpub_call_dst(dst, task, req, buf, lens);
}
void *_svpub_call_resp(task_ctx *task, name_t name, subtype_t req, void *buf, size_t lens,
                       size_t *size, int32_t *erro) {
    task_ctx *dst = task_grab(task->loader, name);
    if (NULL == dst) {
        FREE(buf);
        SET_PTR(size, 0);
        *erro = ERR_FAILED;
        return NULL;
    }
    void *resp = coro_request(dst, task, req, buf, lens, 0, erro, size);
    task_ungrab(dst);
    if (ERR_OK != *erro) {
        SET_PTR(size, 0);
        return NULL;
    }
    return resp;
}
int32_t _svpub_send(task_ctx *task, name_t name, subtype_t req, uint64_t sess,
                    void *buf, size_t lens) {
    task_ctx *dst = task_grab(task->loader, name);
    if (NULL == dst) {
        FREE(buf);
        return ERR_FAILED;
    }
    return _svpub_send_dst(dst, task, req, sess, buf, lens);
}
void _svpub_respond(loader_ctx *loader, name_t src, subtype_t req, uint64_t sess, int32_t erro) {
    if (INVALID_TNAME == src) {
        return;
    }
    task_ctx *dst = task_grab(loader, src);
    if (NULL == dst) {
        return;
    }
    task_response(dst, req, sess, erro, NULL, 0, 0);
    task_ungrab(dst);
}
