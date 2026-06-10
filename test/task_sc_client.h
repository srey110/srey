#ifndef TASK_SC_CLIENT_H_
#define TASK_SC_CLIENT_H_

#include "lib.h"

// subcenter (lib/services/subcenter.c) 集成测试 — 以 client 协程身份调 coro_sc_*:
//   1) 单订阅 + publish 收 1 次,验 topic/payload/publisher
//   2) 通配 "+" 匹配:订 "t2/+",发 "t2/a" "t2/b" 各收 1 次
//   3) 通配 "#" 匹配:订 "t3/#",发 "t3/a" "t3/a/b" 各收 1 次
//   4) 自回环:同 task 自订自发,收到 1 次
//   5) 多 pattern 同 publish 命中(双 pattern 同时订),去重 publish_dedup 收 1 次
//   6) 双角色:同 task 普通订 + 共享订 同 topic,publish 收 2 次(C 层 group 间不去重)
//   7) retained:publish_retained → query_retained 拿到
//   8) publish_retained plen=0 清空,query_retained 拿不到
//   9) retained_meta 快照:set_meta → publish_retained → set_meta 改 → query_retained 拿原快照
//   10) set_meta + publish,deliver 带 meta
//   11) unsubscribe 后不再收
//   12) 重复 sub / 未订阅 unsub 幂等
//   13) topics 调试输出非空
//   14) retained_topics 调试输出非空
//   15) 共享订阅不收 retained:publish_retained 后共享订阅 query 仍可见 retained,但不自动 deliver
// ASan 不报 leak 即证明 sc_topic_data / sc_retained_entry / publisher_meta_entry / 内部 sarray/hashmap/hashset 释放路径正确。
// sc_name 由 main.c 传入,与 sc_start 注册的 name 一致。
void task_sc_client_start(loader_ctx *loader, const char *base_name, const char *sc_name, int32_t *ok);

#endif//TASK_SC_CLIENT_H_
