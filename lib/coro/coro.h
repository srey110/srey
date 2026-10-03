#ifndef CORO_H_
#define CORO_H_

#include "base/base.h"

// 通用协程调度器(基于 minicoro)：只管协程池、按 (sess, tag) 挂起与唤醒、超时、fork、串行锁，
// 不认识 task / 消息 / 网络，用它的一方(owner)经 coro_hooks 接进来。
// 同一个 coro_ctx 上的调用必须串行，由 owner 保证(task 靠调度标志，同一时刻只有一个 worker 在跑它)。
// 协程挂起后若可能在别的线程上恢复，协程代码里的线程局部变量一律经 TLS_DEFINE 的访问器取，
// 也不许跨挂起持有 rwlock_distr / 互斥锁，见 lib/base/macro_util.h。
// sess 条目的五条规则分别落在：keep 只在新建条目时定(coro_wait)；只看队头、摘空且 !keep 才删(coro_wake)；
// 广播先清 keep、全部唤醒、重新查询再删(coro_wake_all)；超时无视 keep(coro_expire)

typedef struct coro_ctx coro_ctx;
typedef struct coro_serial_ctx coro_serial_ctx;
// fork / serial 的用户函数，owner 即 coro_new 传入的那个
typedef void (*coro_fn)(void *owner, void *arg);
// 超时唤醒时由 owner 给到期的等待者造唤醒数据
typedef void *(*coro_timeout_cb)(void *ud, uint64_t sess, int32_t tag);
// owner 钩子，coro_new 时整份拷走
typedef struct coro_hooks {
    void (*run)(void *owner, void *payload); // 新协程入口：跑 coro_spawn 交来的 payload；必须非 NULL
    void (*before_run)(void *owner);         // 协程开跑前调(如给 owner 加引用，保证挂起期间 owner 不被释放)；可为 NULL
    void (*after_run)(void *owner);          // 协程跑完并回池之后调(如释放 before_run 加的引用)，不得在里面释放本调度器(唤醒方返回后还要用它)；可为 NULL
    const char *(*tagstr)(int32_t tag);      // coro_dump 打印标签名；NULL 则打数字
}coro_hooks;

/// <summary>
/// 释放协程后端在本线程上占用的资源，须在跑过协程的线程退出前调用。
/// 只有 Windows 上除 x64 外（32 位与 ARM64）走的 fibers 后端有东西可放（把线程转回非 fiber
/// 态），其余后端是空操作——但 Win32 是 vcxproj 里配着的平台，不是假想配置。
/// 新增会跑协程的线程类别时，记得在它的退出钩子里接上
/// </summary>
void coro_thread_cleanup(void);
/// <summary>
/// 按协程栈的上下界修正栈大小。上下界在 ASan 构建下一起抬高(插桩把每帧撑大数倍)
/// </summary>
/// <param name="stack_size">期望的栈字节数</param>
/// <returns>coro_new 实际会用的栈字节数：0 或小于下界取下界，大于上界取上界，其余原样</returns>
size_t coro_stack_fit(size_t stack_size);
/// <summary>
/// 新建调度器。每个调度器有自己的协程描述(栈大小)与协程池，不同场景互不影响
/// </summary>
/// <param name="hooks">owner 钩子，整份拷贝，调用后可释放；run 必须非 NULL</param>
/// <param name="owner">原样交给各钩子与 coro_fn</param>
/// <param name="stack_size">协程栈字节数，按 coro_stack_fit 修正，不打日志</param>
/// <returns>调度器</returns>
coro_ctx *coro_new(const coro_hooks *hooks, void *owner, size_t stack_size);
/// <summary>
/// 释放调度器：协程池、节点池、超时堆、sess 表，以及还没释放的 serial 与没起的 fork(不跑它们的函数)。
/// 调用时不得还有挂起的协程(owner 应在 before_run 里加引用，挂起者未结束就走不到这里)，否则打一行 WARN，它们连同协程栈泄漏。
/// owner 自己的数据若在析构里还要用 serial，必须先析构那些数据再调本函数
/// </summary>
/// <param name="co">调度器</param>
void coro_free(coro_ctx *co);
/// <summary>
/// 取 coro_new 时传入的 owner
/// </summary>
/// <param name="co">调度器</param>
/// <returns>owner</returns>
void *coro_owner(coro_ctx *co);
/// <summary>
/// 当前是否正跑在本调度器的某个协程里
/// </summary>
/// <param name="co">调度器</param>
/// <returns>1 在协程里；0 不在</returns>
int32_t coro_running(coro_ctx *co);
/// <summary>
/// 挂起中的协程数，含 coro_wait、coro_fork_wait、serial 排队三类
/// </summary>
/// <param name="co">调度器</param>
/// <returns>挂起数</returns>
int32_t coro_suspended(coro_ctx *co);
/// <summary>
/// 从池里取一个协程(池空就新建)，在其中跑 hooks.run(owner, payload)，跑到第一次挂起或结束才返回。
/// 协程内外都可调：在协程内调时，新协程挂起或结束后回到调用方协程接着跑
/// </summary>
/// <param name="co">调度器</param>
/// <param name="payload">交给 run，只保证在 run 第一次挂起前有效；要留过挂起，run 得先自己拷一份</param>
void coro_spawn(coro_ctx *co, void *payload);
/// <summary>
/// 挂起当前协程，等 sess 上标签为 tag 的唤醒。同一 sess 上的等待者严格先进先出。
/// 不在协程里调用会触发断言
/// </summary>
/// <param name="co">调度器</param>
/// <param name="sess">会话 ID</param>
/// <param name="tag">期望的唤醒标签</param>
/// <param name="keep">只在本次新建 sess 条目时生效：非 0 则等待者摘空后条目仍保留，之后只有 coro_wake_all 或超时能删它</param>
/// <param name="ms">超时毫秒，0 不超时；精度取决于 owner 多久调一次 coro_expire</param>
/// <returns>唤醒方给的数据(coro_wake / coro_wake_all 的 payload，或 coro_expire 里 mk 的返回值)，只保证在本协程下次挂起前有效</returns>
void *coro_wait(coro_ctx *co, uint64_t sess, int32_t tag, int32_t keep, uint32_t ms);
/// <summary>
/// 唤醒 sess 上排在队头、标签为 tag 的等待者，被唤醒的协程跑到下次挂起才返回。
/// 队头标签对不上即视为没有等待者，不越过队头往后找；摘掉后队列空且条目不 keep 就删条目
/// </summary>
/// <param name="co">调度器</param>
/// <param name="sess">会话 ID</param>
/// <param name="tag">唤醒标签</param>
/// <param name="payload">交给等待者，由调用方持有，本函数返回后即可释放</param>
/// <returns>ERR_OK 唤醒了一个；ERR_FAILED 没有匹配的等待者(要不要另起协程由调用方定)</returns>
int32_t coro_wake(coro_ctx *co, uint64_t sess, int32_t tag, void *payload);
/// <summary>
/// 广播(连接关闭用)：条目的 keep 先清零(不可逆)，再按顺序唤醒 sess 上全部等待者(不看标签)，
/// spawn 非 NULL 时随后起一个协程跑它，最后重新查一次条目，空了才删。
/// 起协程必须排在重新查询之前：新协程若在同一 sess 上挂起，得追加进这个已经 keep=0 的条目，
/// 否则会新建一个 keep 条目，而该 sess 的广播已经过去，再没有路径能删它
/// </summary>
/// <param name="co">调度器</param>
/// <param name="sess">会话 ID</param>
/// <param name="payload">交给每个等待者，由调用方持有</param>
/// <param name="spawn">非 NULL 时等同 coro_spawn(co, spawn)；NULL 不起协程</param>
void coro_wake_all(coro_ctx *co, uint64_t sess, void *payload, void *spawn);
/// <summary>
/// 唤醒全部已到期的等待者：按到期先后从超时堆取，摘掉后队列空就删条目(无视 keep：留着的话
/// 该 sess 的广播若已过去，条目再没有路径能删)，再用 mk(ud, sess, tag) 的返回值唤醒它。
/// 顺带按周期收缩协程池。由 owner 定时调用
/// </summary>
/// <param name="co">调度器</param>
/// <param name="mk">给每个到期者造唤醒数据；可以每次返回同一个指针，每个到期者只用到它下次挂起。NULL 则到期者拿到 NULL</param>
/// <param name="ud">透传给 mk</param>
void coro_expire(coro_ctx *co, coro_timeout_cb mk, void *ud);
/// <summary>
/// 在新协程中执行 fn(owner, arg)，fire-and-forget，当前协程不让出：
/// 先排进待起队列，由 owner 下一次 coro_fork_drain 起协程。
/// 仅可在本调度器的协程内调用(否则告警并忽略)。
/// arg 由调用方管理生命周期；fn 内部 abort/segfault 终止进程(C 无 xpcall 兜底)
/// </summary>
/// <param name="co">调度器</param>
/// <param name="fn">协程函数</param>
/// <param name="arg">透传给 fn</param>
void coro_fork(coro_ctx *co, coro_fn fn, void *arg);
/// <summary>
/// 并发执行 n 个 fns[i](owner, args[i])，全部完成后才返回(barrier)，总耗时约为最慢那个。
/// 调用方必须身处本调度器的协程内。C 无闭包：各函数的结果须自己写进 args[i] 里的字段
/// </summary>
/// <param name="co">调度器</param>
/// <param name="fns">长度为 n 的函数数组</param>
/// <param name="args">长度为 n 的参数数组，与 fns 一一对应</param>
/// <param name="n">个数；小于等于 0 立即返回 ERR_OK</param>
/// <returns>ERR_OK 全部完成；ERR_FAILED 调用方不在协程内</returns>
int32_t coro_fork_wait(coro_ctx *co, coro_fn fns[], void *args[], int32_t n);
/// <summary>
/// 把待起队列跑到空(跑的过程中新 fork 的追加在队尾，一并跑掉)。
/// owner 在每次分发结束时调用一次
/// </summary>
/// <param name="co">调度器</param>
void coro_fork_drain(coro_ctx *co);
/// <summary>
/// 创建协程串行化执行器(critical section)：同一调度器内多协程对同一资源并发访问时串行进入，
/// 跨协程按先进先出排队；同一协程嵌套调用安全(ref 计数)
/// </summary>
/// <param name="co">调度器</param>
/// <returns>coro_serial_ctx，销毁用 coro_serial_free</returns>
coro_serial_ctx *coro_serial_new(coro_ctx *co);
/// <summary>
/// 销毁串行化执行器：排队中的等待者被逐个唤醒并失败返回(锁不交接)，此后 enter 一律失败。
/// 允许在有协程持锁时调用(含持锁者自己)；锁抢不走，内存改由最后一次 coro_serial_leave 释放，
/// **故本函数返回时对象未必已经释放**。调用方两条义务：
/// 1) 把自己的 serial 字段置空要排在本函数之后，且置空前先认字段仍是自己那个——
///    期间可能已销毁重连、装上新执行器，无条件置空会把新的抹掉；
/// 2) 加锁与解锁必须捏同一个指针配对，解锁时不得重读已被置空的字段——
///    重读会让持锁者跳过 leave，推迟的释放就永远等不到
/// </summary>
/// <param name="serial">coro_serial_ctx</param>
void coro_serial_free(coro_serial_ctx *serial);
/// <summary>
/// 进入临界区。调用方必须身处协程内。
/// 同协程嵌套安全(ref 计数)；跨协程时按 FIFO 排队挂起，前一个 leave 时唤醒下一个。
/// 配对由调用方保证：enter 成功后到 leave 之间的任何提前 return 都会把锁永久漏掉，
/// 所以两者之间不要写早退分支，写不下就改用 coro_serial_call
/// </summary>
/// <param name="serial">coro_serial_ctx</param>
/// <returns>ERR_OK 已持锁，调用方必须配对调用 coro_serial_leave；
/// ERR_FAILED 未持锁，不得调用 leave(不在协程内、或该执行器正在 coro_serial_free 销毁)</returns>
int32_t coro_serial_enter(coro_serial_ctx *serial);
/// <summary>
/// 离开临界区，仅在 coro_serial_enter 返回 ERR_OK 后调用。
/// ref 归 0 时就地唤醒队头等待者(minicoro 切栈，链式唤醒不累积 C 栈)；
/// 若临界区期间有人调过 coro_serial_free，本次 ref 归 0 即在此释放对象——返回后不得再碰 serial
/// </summary>
/// <param name="serial">coro_serial_ctx</param>
void coro_serial_leave(coro_serial_ctx *serial);
/// <summary>
/// coro_serial_enter + fn(owner, arg) + coro_serial_leave 的回调式写法，语义与分体式一致。
/// 配对由本函数保证，故 fn 内可随意早退。
/// C 无 xpcall：fn 内 abort 终止进程，调用方自行保证 fn 不崩
/// </summary>
/// <param name="serial">coro_serial_ctx</param>
/// <param name="fn">临界区函数；NULL 则只做一次进出，用于探测能否拿到锁</param>
/// <param name="arg">透传给 fn(生命周期由调用方管理)</param>
/// <returns>ERR_OK 成功；ERR_FAILED 调用方不在协程内、或该执行器正在 coro_serial_free 销毁</returns>
int32_t coro_serial_call(coro_serial_ctx *serial, coro_fn fn, void *arg);
/// <summary>
/// 转储挂起协程为文本 buffer(调试用)。C 协程无栈回溯，能给的只有等待原因与时长。
/// 三类挂起分别列出：等唤醒的(sess/标签/时长)、等 fork_wait 的(未完成子协程数)、
/// 等 serial 交接的(执行器地址/持锁协程/持锁多久/排队人数与最久那个排了多久)——空闲 serial
/// 不出行；排队时长只在真有人排队时才给。挂起段与 serial 段都带 co=，靠它把"谁占着锁"
/// 和"那个协程卡在哪"对上号。末行四个计数满足 suspended + fork_wait + serial == yield total，
/// 与 coro_suspended 对得上号
/// </summary>
/// <param name="co">调度器</param>
/// <param name="size">出参：buffer 字节数；NULL 不写</param>
/// <returns>文本 buffer，所有权转给调用方，用完 FREE</returns>
char *coro_dump(coro_ctx *co, size_t *size);

#endif//CORO_H_
