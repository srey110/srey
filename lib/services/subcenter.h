#ifndef SUBCENTER_H_
#define SUBCENTER_H_

#include "srey/coro.h"
#include "path/path_rules.h"
#include "utils/binary.h"

// subcenter:订阅中心 service,task 间 pub/sub
// 命名约定:
//   类型 / 函数统一 sc_ 短前缀(协程版前再加 coro_ 前缀)
//   常量 SC_ 大写前缀(SC_RETAINED_MAX_SIZE 等)
// 内部存储分离:
//   - 订阅关系挂 path_trie 节点 payload(sc_topic_data)
//   - retained 保留消息挂独立 hashmap<topic, sc_retained_entry>
//   两者职责分明,互不干扰:SUB/UNSUB 永远不动 retained_index;
//   publish(非 retained)永远不动 retained_index;
//   publish_retained 先更新 retained_index 再走普通 publish 投递路径。
// 使用范式:
//   (1) 纯 task 间 pub/sub:subscribe / publish / publish_retained;
//       想立即查 retained 调 query_retained;Lua wrapper subscribe 内部自动两步。
//       publisher 可 set_meta 注册元数据,所有 publish 自动携带 meta 投递给订阅者。
//   (2) 网络订阅网关:网关作为单一订阅者代理 N 个网络客户端,
//       网关本地维护 fd ↔ pattern 映射,用 path_trie 做反向匹配;
//       共享订阅:subcenter 是 task 粒度,客户端粒度共享需网关本地 fd 轮询;
//       publisher 字段在网关场景永远是网关 task,客户端身份用 set_meta 或协议层 properties 传递。
// 注意点:
//   1. 无 retained TTL,孤儿 retained 需业务显式 publish_retained plen=0 清空
//   2. query_retained O(M) 已用 retained_index 直查(已优化),M = retained 总数

// REQ_SC_DELIVER 投递来源(sc_deliver.kind);普通订阅与共享订阅各自独立投递,接收方据此路由
typedef enum sc_deliver_kind {
    SC_DELIVER_NORMAL = 0, // 普通订阅投递
    SC_DELIVER_SHARED = 1  // 共享订阅投递
} sc_deliver_kind;
// REQ_SC_DELIVER 推送 wire 解析结果；topic/payload/meta/group 零拷贝指向源 data,生命周期与 data 一致。
// topic/payload/meta/group 非 NUL 结尾,须配合对应 len 使用。
typedef struct sc_deliver {
    int32_t kind;         // 投递来源:SC_DELIVER_NORMAL(普通) / SC_DELIVER_SHARED(共享)
    size_t tlen;          // topic 字节数
    size_t plen;          // 载荷字节数
    size_t mlen;          // 元数据字节数
    size_t glen;          // 组名字节数;0 表示普通投递无组
    name_t publisher;     // 发布者 task 句柄;INVALID_TNAME 表示 publisher 已失效
    const char *topic;    // 匹配到的精确 topic(非 NUL 结尾)
    const char *payload;  // 载荷;plen=0 时 NULL
    const char *meta;     // 发布者元数据;mlen=0 时 NULL
    const char *group;    // 共享投递的组名(kind=SHARED 时非 NULL;NORMAL 时 NULL)
} sc_deliver;
// query_retained / topics / retained_topics 响应的逐条解析结果;指针零拷贝指向源 data,生命周期与 data 一致,非 NUL 结尾。
typedef struct sc_retained {
    size_t mlen;          // 元数据字节数
    size_t tlen;          // topic 字节数
    size_t plen;          // 载荷字节数
    name_t publisher;     // 原 retained 发布者句柄;INVALID_TNAME 表示已失效
    const char *meta;     // 发布者元数据;mlen=0 时 NULL
    const char *topic;    // retained topic;tlen=0 时 NULL
    const char *payload;  // retained 载荷;plen=0 时 NULL
} sc_retained;
typedef struct sc_topic {
    uint32_t normal;      // 普通订阅者数
    uint32_t shared;      // 共享订阅组数
    size_t tlen;          // topic 字节数
    const char *topic;    // topic;tlen=0 时 NULL
} sc_topic;
typedef struct sc_retained_topic {
    uint16_t meta_size;   // retained meta 字节数
    uint32_t size;        // retained 载荷字节数
    size_t tlen;          // topic 字节数
    name_t publisher;     // retained 发布者句柄;INVALID_TNAME 表示已失效
    const char *topic;    // topic;tlen=0 时 NULL
} sc_retained_topic;
/// <summary>
/// 注册 subcenter task service。
/// 在 loader_init 之后、业务 task 启动之前调用一次。
/// name 由 config.json 的 sc_name 决定,默认 "subcenter";空串表示不启动。
/// </summary>
/// <param name="loader">loader_ctx</param>
/// <param name="name">字符串任务名;NULL 或空串时本函数立即返回 ERR_OK 不注册 task</param>
/// <param name="rules">topic 规则;调用方先调 path_rules_def/path_rules_mqtt 填充再传入,
///     也可在填充后追加 validate_segment/validate_path 自定义校验。
///     NULL 时返 ERR_FAILED。规则生命周期需 ≥ subcenter task</param>
/// <returns>ERR_OK 成功(含跳过);ERR_FAILED 注册失败</returns>
int32_t sc_start(loader_ctx *loader, const char *name, const path_rules *rules);
/// <summary>
/// 订阅 topic(可含通配)。重复订阅相同 src+topic 幂等返 OK。必须在协程中调用。
/// </summary>
/// <param name="task">当前 task(订阅者身份)</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="topic">订阅模式;可含通配符(由 rules 配置)</param>
/// <returns>ERR_OK 成功;ERR_FAILED topic 非法 / subcenter 不可达 / 分配失败</returns>
int32_t coro_sc_subscribe(task_ctx *task, name_t sc_name, const char *topic);
/// <summary>
/// 共享订阅:同 group 内多个订阅者轮询接收 publish。不收 retained。必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="topic">订阅模式;可含通配符</param>
/// <param name="group">共享组名;非空,上限 SC_GROUP_MAX</param>
/// <returns>ERR_OK 成功;ERR_FAILED 参数非法 / subcenter 不可达 / 分配失败</returns>
int32_t coro_sc_subscribe_shared(task_ctx *task, name_t sc_name,
                                 const char *topic, const char *group);
/// <summary>取消订阅。未订阅过的 topic 幂等返 OK。必须在协程中调用。</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="topic">订阅模式;须与 subscribe 时完全一致才能命中</param>
/// <returns>ERR_OK 成功;ERR_FAILED topic 非法 / subcenter 不可达</returns>
int32_t coro_sc_unsubscribe(task_ctx *task, name_t sc_name, const char *topic);
/// <summary>取消共享订阅。未订阅过的 topic+group 幂等返 OK。必须在协程中调用。</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="topic">订阅模式;须与 subscribe_shared 时完全一致</param>
/// <param name="group">共享组名;须与 subscribe_shared 时完全一致</param>
/// <returns>ERR_OK 成功;ERR_FAILED 参数非法 / subcenter 不可达</returns>
int32_t coro_sc_unsubscribe_shared(task_ctx *task, name_t sc_name,
                                   const char *topic, const char *group);
/// <summary>
/// 发布消息到精确 topic。fire-and-forget,subcenter 不挂状态,投递路径上 task_grab 失败的
/// 订阅者被懒清理。必须在协程中调用。
/// </summary>
/// <param name="task">当前 task(publisher 身份,deliver wire 中 publisher 字段值)</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="topic">精确 topic;不允许含通配</param>
/// <param name="data">消息 payload(允许 NULL)</param>
/// <param name="size">payload 字节数;上限 UINT32_MAX</param>
/// <returns>ERR_OK 成功投递;ERR_FAILED topic 非法 / subcenter 不可达</returns>
int32_t coro_sc_publish(task_ctx *task, name_t sc_name, const char *topic,
                        void *data, size_t size);
/// <summary>
/// 发布保留消息。retained_index 记录原 publisher + meta 快照,新订阅者通过 query_retained 拿到。
/// plen=0 等价"清空 retained 槽位,不 deliver"。retained 上限 SC_RETAINED_MAX_SIZE。
/// 普通订阅者同时收到 deliver,共享订阅不收。必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="topic">精确 topic;不允许含通配</param>
/// <param name="data">retained payload(允许 NULL;NULL/size=0 时清空 retained 槽位)</param>
/// <param name="size">payload 字节数;> SC_RETAINED_MAX_SIZE 时拒绝</param>
/// <returns>ERR_OK 成功;ERR_FAILED 参数非法 / subcenter 不可达 / 超长</returns>
int32_t coro_sc_publish_retained(task_ctx *task, name_t sc_name, const char *topic,
                                 void *data, size_t size);
/// <summary>
/// 查询匹配 pattern 的所有当前 retained 消息(主动查询,不订阅)。
/// 单次返回上限 SC_QUERY_RETAINED_BURST_MAX,超过截断 + LOG_WARN。必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="pattern">查询模式;可含通配</param>
/// <param name="size">出参:返回 buffer 字节数;NULL 不写</param>
/// <param name="erro">出参:ERR_OK 成功(含无匹配);ERR_FAILED subcenter 不可达/超时</param>
/// <returns>多条 retained 拼接 buffer,下次 yield 前有效;每条格式:
///     | name_t retained_publisher | u16 mlen | meta | u16 tlen | topic | u32 plen | payload |
///     无匹配返 NULL 且 size=0(erro=ERR_OK),失败返 NULL 且 erro=ERR_FAILED;
///     用 sc_parse_retained 逐条解析</returns>
void *coro_sc_query_retained(task_ctx *task, name_t sc_name, const char *pattern,
                             size_t *size, int32_t *erro);
/// <summary>
/// 列出所有订阅 topic(仅订阅信息,不含 retained)。调试用,topic 量大时谨慎调。
/// 必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="size">出参:返回 buffer 字节数;NULL 不写</param>
/// <param name="erro">出参:ERR_OK 成功(含空);ERR_FAILED subcenter 不可达/超时</param>
/// <returns>binary buffer,每条格式:
///     | u16 tlen | topic | u32 normal_count | u32 shared_groups_count |
///     空时返 NULL 且 size=0(erro=ERR_OK),失败返 NULL 且 erro=ERR_FAILED;下次 yield 前有效;
///     用 sc_parse_topics 逐条解析</returns>
void *coro_sc_topics(task_ctx *task, name_t sc_name,
                     size_t *size, int32_t *erro);
/// <summary>
/// 列出所有 retained topic 元信息(不返 retained payload 自身,避免数据量大)。
/// 调试用。必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="size">出参:返回 buffer 字节数;NULL 不写</param>
/// <param name="erro">出参:ERR_OK 成功(含空);ERR_FAILED subcenter 不可达/超时</param>
/// <returns>binary buffer,每条格式:
///     | u16 tlen | topic | name_t retained_publisher | u32 retained_size | u16 retained_meta_size |
///     空时返 NULL 且 size=0(erro=ERR_OK),失败返 NULL 且 erro=ERR_FAILED;下次 yield 前有效;
///     用 sc_parse_retained_topics 逐条解析</returns>
void *coro_sc_retained_topics(task_ctx *task, name_t sc_name,
                              size_t *size, int32_t *erro);
/// <summary>
/// 注册或更新当前 task 的发布者元数据。
/// 后续该 task 所有 publish/publish_retained 都自动携带 meta 投递给订阅者。
/// publisher 应在 _closing 钩子调 set_meta(NULL, 0) 主动清理。必须在协程中调用。
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="meta">元数据 buffer(允许 NULL)</param>
/// <param name="size">字节数;0 等价清除元数据;> SC_META_MAX_SIZE 返 ERR_FAILED</param>
/// <returns>ERR_OK 成功;ERR_FAILED subcenter 不可达</returns>
int32_t coro_sc_set_meta(task_ctx *task, name_t sc_name,
                         const void *meta, size_t size);
// ── 异步版 API ────────────────────────────────────────────────────────────
// 非协程上下文使用;不挂起。sess 必须非 0(=0 返 ERR_FAILED),业务自管响应配对,
// 在 task->_response 回调中按 sess 收 OK / 数据。语义与协程版完全一致,差别仅在阻塞与否。

/// <summary>异步订阅。同 coro_sc_subscribe,但不挂起</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0(=0 返 ERR_FAILED),业务自管响应配对</param>
/// <param name="topic">订阅模式</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_subscribe(task_ctx *task, name_t sc_name, uint64_t sess, const char *topic);
/// <summary>异步共享订阅。同 coro_sc_subscribe_shared,但不挂起</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0,=0 返 ERR_FAILED</param>
/// <param name="topic">订阅模式</param>
/// <param name="group">共享组名</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_subscribe_shared(task_ctx *task, name_t sc_name, uint64_t sess,
                            const char *topic, const char *group);
/// <summary>异步取消订阅。同 coro_sc_unsubscribe,但不挂起</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0,=0 返 ERR_FAILED</param>
/// <param name="topic">订阅模式</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_unsubscribe(task_ctx *task, name_t sc_name, uint64_t sess, const char *topic);
/// <summary>异步取消共享订阅。同 coro_sc_unsubscribe_shared,但不挂起</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0,=0 返 ERR_FAILED</param>
/// <param name="topic">订阅模式</param>
/// <param name="group">共享组名</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_unsubscribe_shared(task_ctx *task, name_t sc_name, uint64_t sess,
                              const char *topic, const char *group);
/// <summary>异步发布。同 coro_sc_publish,但不挂起</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0,=0 返 ERR_FAILED</param>
/// <param name="topic">精确 topic</param>
/// <param name="data">payload</param>
/// <param name="size">payload 字节数</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_publish(task_ctx *task, name_t sc_name, uint64_t sess, const char *topic,
                   void *data, size_t size);
/// <summary>异步发布保留消息。同 coro_sc_publish_retained,但不挂起</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0,=0 返 ERR_FAILED</param>
/// <param name="topic">精确 topic</param>
/// <param name="data">retained payload(NULL/0 清空槽位)</param>
/// <param name="size">payload 字节数</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_publish_retained(task_ctx *task, name_t sc_name, uint64_t sess,
                            const char *topic, void *data, size_t size);
/// <summary>
/// 异步查询匹配 pattern 的当前 retained。同 coro_sc_query_retained,但不挂起;
/// 业务在 _response 回调里按 sess 收 buffer,用 sc_parse_retained 逐条解析
/// </summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0,=0 返 ERR_FAILED</param>
/// <param name="pattern">查询模式</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_query_retained(task_ctx *task, name_t sc_name, uint64_t sess, const char *pattern);
/// <summary>异步列出订阅 topic。同 coro_sc_topics,但不挂起;响应 buffer 用 sc_parse_topics 逐条解析</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0,=0 返 ERR_FAILED</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_topics(task_ctx *task, name_t sc_name, uint64_t sess);
/// <summary>异步列出 retained 元信息。同 coro_sc_retained_topics,但不挂起;响应 buffer 用 sc_parse_retained_topics 逐条解析</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0,=0 返 ERR_FAILED</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_retained_topics(task_ctx *task, name_t sc_name, uint64_t sess);
/// <summary>异步设置 publisher meta。同 coro_sc_set_meta,但不挂起</summary>
/// <param name="task">当前 task</param>
/// <param name="sc_name">subcenter task name</param>
/// <param name="sess">会话 id;必须非 0,=0 返 ERR_FAILED</param>
/// <param name="meta">元数据 buffer(NULL/0 清除)</param>
/// <param name="size">字节数;> SC_META_MAX_SIZE 返 ERR_FAILED</param>
/// <returns>ERR_OK 投递成功;ERR_FAILED subcenter 不可达</returns>
int32_t sc_set_meta(task_ctx *task, name_t sc_name, uint64_t sess,
                    const void *meta, size_t size);
// ── deliver 解析 ──────────────────────────────────────────────────────────
/// <summary>
/// 解析 REQ_SC_DELIVER 推送 wire(订阅者在 _request 回调中调用)。wire 首字节为 kind
/// (SC_DELIVER_NORMAL/SC_DELIVER_SHARED),区分普通投递与共享投递,填入 out->kind。
/// 出参 topic/payload/meta 零拷贝指向 data,不可在 data 失效后使用。
/// </summary>
/// <param name="data">REQ_SC_DELIVER 消息 data</param>
/// <param name="size">data 字节数</param>
/// <param name="out">出参:解析结果,成功时填充</param>
/// <returns>ERR_OK 成功;ERR_FAILED wire 不完整(损坏/截断)</returns>
int32_t sc_parse_deliver(const void *data, size_t size, sc_deliver *out);
// ── 列表响应解析 ──────────────────────────────────────────────────────────
// 游标式逐条解析 query_retained / topics / retained_topics 的多记录响应 buffer。
// 调用方先 binary_init(&br, data, size, 0),再循环 while(br.offset < br.size) 取下一条;出参指针零拷贝指向源 buffer。
/// <summary>
/// 解析 br 当前位置的下一条 retained。出参 meta/topic/payload 零拷贝指向 br 源 buffer。
/// </summary>
/// <param name="br">query_retained 响应 binary_ctx(已 init);成功后内部 offset 推进到下一条</param>
/// <param name="out">出参:解析结果,成功时填充</param>
/// <returns>ERR_OK 解析出一条;ERR_FAILED 当前位置数据不足一条(截断/损坏)</returns>
int32_t sc_parse_retained(binary_ctx *br, sc_retained *out);
/// <summary>解析 br 当前位置的下一条 topic 订阅统计。out->topic 零拷贝指向源 buffer。</summary>
/// <param name="br">topics 响应 binary_ctx(已 init);成功后 offset 推进到下一条</param>
/// <param name="out">出参:解析结果,成功时填充</param>
/// <returns>ERR_OK 解析出一条;ERR_FAILED 数据不足一条(截断/损坏)</returns>
int32_t sc_parse_topics(binary_ctx *br, sc_topic *out);
/// <summary>解析 br 当前位置的下一条 retained 元信息。out->topic 零拷贝指向源 buffer。</summary>
/// <param name="br">retained_topics 响应 binary_ctx(已 init);成功后 offset 推进到下一条</param>
/// <param name="out">出参:解析结果,成功时填充</param>
/// <returns>ERR_OK 解析出一条;ERR_FAILED 数据不足一条(截断/损坏)</returns>
int32_t sc_parse_retained_topics(binary_ctx *br, sc_retained_topic *out);

#endif//SUBCENTER_H_
