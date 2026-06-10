#ifndef DEBUG_REQUEST_H_
#define DEBUG_REQUEST_H_

#include "srey/task.h"

// 公共debug信息处理,如果处理了该请求不管是否成功都返回ERR_OK
int32_t _debug_request(task_ctx *task, message_ctx *msg);

#endif//DEBUG_REQUEST_H_
