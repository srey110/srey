#ifndef CTASK_H_
#define CTASK_H_

#include "lib.h"

typedef struct ctask_ctx ctask_ctx;
typedef void(*message_cb)(ctask_ctx *ctask, task_ctx *task, message_ctx *msg);
typedef void(*timeout_cb)(ctask_ctx *ctask, task_ctx *task, void *arg);

ctask_ctx *ctask_new(void);
void ctask_regcb(ctask_ctx *ctask, msg_type mtype, message_cb cb);
int32_t ctask_register(srey_ctx *ctx, ctask_ctx *ctask, ctask_ctx *parent, name_t name, uint16_t maxcnt, uint16_t maxmsgqulens);
void ctask_timeout(ctask_ctx *ctask, uint32_t ms, int32_t once, timeout_cb _timeout, free_cb _arg_free, void *arg);

#endif//CTASK_H_
