#ifndef TASK_MULTI_CALL_H_
#define TASK_MULTI_CALL_H_

#include "lib.h"

// task_multi_call + task_multi_request 集成测试：
//   1) 注册 N=5 个 subscriber task,每个 task 装 _requested 回调累计 received_call/received_rpc
//   2) publisher task 在 startup 内 task_grab 到 N 个 subscriber 的 task_ctx*
//   3) 第 1 段：task_multi_call(REQ_TYPE_CALL) 广播,等所有 sub received_call == N
//   4) 第 2 段：task_multi_request(REQ_TYPE_RPC, RPC_SESS) 广播,subscriber 各自 task_response
//      回带 "ack",publisher 的 _response 回调被调 N 次累计 responded_count
//   5) 验证 received_rpc == N && responded_count == N (subscriber 都收到 + publisher 都收到响应)
//   6) 同时验证 dsts 中混入 NULL 占位时被正确跳过
// ASan 下不报 leak 即证明 shared_data 引用计数路径在 call/request 两种语义下都正确。
void task_multi_call_start(loader_ctx *loader, const char *base_name, int32_t *ok);

#endif//TASK_MULTI_CALL_H_
