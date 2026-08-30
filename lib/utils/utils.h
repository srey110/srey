#ifndef UTILS_H_
#define UTILS_H_

#include "base/structs.h"// buf_ctx:split 的段数组元素

// split 的切分标志,按位或。trim 恒在判空之前:" " 这种全空白段原始长度是 1,
// 先判空就漏过去了(RFC 7230 §7 的 list 语义正是要求先 trim 再忽略空元素)
#define SPLIT_TRIM      0x01 //每段剔除两端空字节(SP/HTAB)
#define SPLIT_SKIPEMPTY 0x02 //丢弃空段(与 SPLIT_TRIM 同用时按 trim 后的长度判)
#define SPLIT_TRUNCATE  0x04 //仅栈模式:段数超 cap 时截断到 cap 正常返回,而不是 ERR_FAILED

/// <summary>
/// 设置服务器唯一id（用作 createid 的高16位），须在 createid 首次调用前于启动期设置一次
/// </summary>
/// <param name="id">服务器id，须小于 0x8000</param>
/// <returns>ERR_OK 成功；id 不小于 0x8000 时返回 ERR_FAILED</returns>
int32_t serviceid(uint16_t id);
/// <summary>
/// 获取全局唯一ID：高16位为服务器id(serviceid)，低48位为进程内自增计数
/// </summary>
/// <returns>ID</returns>
uint64_t createid(void);
/// <summary>
/// 从 createid 生成的 ID 中解析出服务器 id（高 16 位）
/// </summary>
/// <param name="id">createid 返回的 ID</param>
/// <returns>服务器 id（与 serviceid 设置值一致，范围 0..0x7FFF）</returns>
uint16_t parse_svid(uint64_t id);
/// <summary>
/// 当前线程ID
/// </summary>
/// <returns>线程ID</returns>
uint64_t threadid(void);
/// <summary>
/// 启coredump socket链接数限制
/// </summary>
void unlimit(void);
/// <summary>
/// 信号处理。cb 在信号处理上下文中被调用，内部只能用 async-signal-safe 函数
/// （LOG_INFO / mutex_lock / malloc 都不行）；安全做法是 cb 里只置
/// volatile sig_atomic_t 标志，主循环轮询后再处理。
/// </summary>
/// <param name="cb">处理函数（必须 async-signal-safe）</param>
/// <param name="data">参数</param>
void sighandle(void(*cb)(int32_t, void *), void *data);
/// <summary>
/// cpu核心数
/// </summary>
/// <returns>核心数</returns>
uint32_t procscnt(void);
/// <summary>
/// 是否为文件
/// </summary>
/// <param name="file">路径</param>
/// <returns>ERR_OK 文件</returns>
int32_t isfile(const char *file);
/// <summary>
/// 是否为文件夹
/// </summary>
/// <param name="path">路径</param>
/// <returns>ERR_OK 文件夹</returns>
int32_t isdir(const char *path);
/// <summary>
/// 文件大小
/// </summary>
/// <param name="file">路径</param>
/// <returns>文件大小, ERR_FAILED 失败</returns>
int64_t filesize(const char *file);
/// <summary>
/// 获取文件最后修改时间
/// </summary>
/// <param name="file">路径</param>
/// <returns>修改时间(秒);失败返回 0</returns>
uint64_t file_mtime(const char *file);
/// <summary>
/// 当前程序所在路径
/// </summary>
/// <returns>路径</returns>
const char *procpath(void);
/// <summary>
/// 读取文件全部。所有失败路径都会置 errno，调用方可直接 strerror——空文件与读取途中被截断
/// 这两种情形本身不来自系统调用，函数会自己补上 errno，不会留下上一次系统调用的陈旧值
/// </summary>
/// <param name="file">路径</param>
/// <param name="lens">文件大小；仅成功时被写入</param>
/// <returns>文件内容，需调用方 FREE；失败返回 NULL 并置 errno</returns>
char *readall(const char *file, size_t *lens);
/// <summary>
/// timeofday。注意 Windows 的 struct timeval.tv_sec 是 32 位 long,2038-01-19 后回绕,
/// 这是该结构体自身的容量上限;需要不截断的时间戳请用 nowms / nowsec
/// </summary>
/// <param name="tv">timeval</param>
void timeofday(struct timeval *tv);
/// <summary>
/// 与UTC时差
/// </summary>
/// <returns>分。系统时间换算失败时返 0(按 UTC 处理),不另设错误码</returns>
int32_t timeoffset(void);
/// <summary>
/// 当前时间戳
/// </summary>
/// <returns>毫秒</returns>
uint64_t nowms(void);
/// <summary>
/// 当前时间戳
/// </summary>
/// <returns>秒</returns>
uint64_t nowsec(void);
/// <summary>
/// 格式化输出时间戳；失败时 time[0] = '\0'
/// </summary>
/// <param name="sec">秒</param>
/// <param name="fmt">格式化 %Y-%m-%d %H:%M:%S</param>
/// <param name="time">时间字符串</param>
/// <returns>ERR_OK 成功，ERR_FAILED 失败</returns>
int32_t sectostr(uint64_t sec, const char *fmt, char time[TIME_LENS]);
/// <summary>
/// 格式化输出时间戳；失败时 time[0] = '\0'
/// </summary>
/// <param name="ms">毫秒</param>
/// <param name="fmt">格式化 %Y-%m-%d %H:%M:%S</param>
/// <param name="time">时间字符串</param>
/// <returns>ERR_OK 成功；ERR_FAILED 失败（时间换算失败，或 fmt 太长以致
///     " 000" 毫秒后缀在 TIME_LENS 内装不下——不截断，整体失败）</returns>
int32_t mstostr(uint64_t ms, const char *fmt, char time[TIME_LENS]);
/// <summary>
/// 字符串转时间戳。
/// fmt 里的 %z / %Z 只吃掉时区文本，偏移量不生效，一律按本地时间 mktime：
/// 带时区的串解析结果会差"串里的偏移 − 本地偏移"秒，要换算的调用方自己补回去
/// </summary>
/// <param name="time">时间字符串</param>
/// <param name="fmt">格式化</param>
/// <returns>时间戳</returns>
uint64_t strtots(const char *time, const char *fmt);
/// <summary>
/// hash
/// </summary>
/// <param name="buf">要计算的数据</param>
/// <param name="len">数据长度</param>
/// <returns>hash</returns>
uint64_t hash(const char *buf, size_t len);
/// <summary>
/// 64 位整数专用哈希（splitmix64
/// </summary>
/// <param name="x">整型 key</param>
/// <returns>hash</returns>
static inline uint64_t hash_u64(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
/// <summary>
/// 判断是否为合法 RFC 7230 token（全部字符为 tchar）。
/// 头名、WebSocket 子协议名等都按此校验：只挡 NUL/CRLF 不够，
/// 键里混进 ':' 或 ' ' 同样会被对端拆成两个字段（走私），故按整个 tchar 集合校验
/// </summary>
/// <param name="data">源数据(可非 NUL 结尾)</param>
/// <param name="lens">源数据长度</param>
/// <returns>是合法 token 返回 1，否则 0（空串不是 token）</returns>
int32_t is_token(const char *data, size_t lens);
/// <summary>
/// 初始化用于 locale 无关数值解析的 C locale 句柄，须在启动期单线程调用一次（strtod_c 依赖）。
/// 创建失败直接 abort：句柄留空会让 strtod_l 在 glibc 上解引用空句柄崩在 DB 解包路径里，
/// 而 Darwin 会静默回落到当前 locale，恰好抹掉 strtod_c 存在的意义
/// </summary>
void locale_init(void);
/// <summary>
/// 释放 locale_init 创建的 C locale 句柄；调用后句柄置空，不可再调 strtod_c
/// </summary>
void locale_free(void);
/// <summary>
/// 按 C locale 解析 double（小数点恒为 '.'），不受进程 LC_NUMERIC 影响
/// </summary>
/// <param name="str">NUL 结尾数值字符串</param>
/// <param name="endptr">输出：解析停止位置</param>
/// <returns>解析出的 double</returns>
double strtod_c(const char *str, char **endptr);
/// <summary>
/// 随机[min, max]
/// </summary>
/// <param name="min">最小</param>
/// <param name="max">最大</param>
/// <returns>值</returns>
int32_t randrange(int32_t min, int32_t max);
/// <summary>
/// 随机字符串
/// </summary>
/// <param name="buf">buffer，必须至少分配 len+1 字节（末尾写 '\0'）</param>
/// <param name="len">随机字符数</param>
/// <returns>char *</returns>
char *randstr(char *buf, size_t len);
/// <summary>
/// 按 sep 拆分,仅记录段 (data,lens) 不复制,data 指向 ptr 内部。
/// cap 决定内存模型:cap 为 0 走堆(函数分配,segs 是纯出参,不读它的旧值),
/// cap 大于 0 走调用方的栈数组
/// </summary>
/// <param name="ptr">待拆分缓冲(只读取,不修改)</param>
/// <param name="plens">ptr 字节长度</param>
/// <param name="sep">分隔符;NULL 或 seplens 为 0 时整段不切,只出一段</param>
/// <param name="seplens">分隔符字节数</param>
/// <param name="segs">cap 为 0 时输出新分配的段数组(调用方 FREE);cap 大于 0 时传入调用方数组,须非 NULL</param>
/// <param name="cap">0 走堆(上限 INT32_MAX 段,即返回值本身的上限);大于 0 即栈数组容量</param>
/// <param name="flags">SPLIT_* 按位或,0 为标准切分</param>
/// <returns>段数量;plens 为 0 时出一个空段(仍是"sep 出现次数 + 1")。
///   ERR_FAILED 表示参数非法(ptr/segs 为空、cap 为负、栈模式 *segs 为空),
///   栈模式超 cap 且未开 SPLIT_TRUNCATE(segs 内容未定义),
///   或堆模式段数达 INT32_MAX(*segs 已分配,仍须 FREE)。
///   cap 判定排在过滤之后,被 SPLIT_SKIPEMPTY 丢掉的段不占名额;
///   堆模式开 SPLIT_SKIPEMPTY 后可能返回 0,但 *segs 已分配,仍须 FREE。
///   空段记录原指针与 lens 为 0(不是 NULL)</returns>
int32_t split(char *ptr, size_t plens, const char *sep, size_t seplens,
              buf_ctx **segs, int32_t cap, int32_t flags);
/// <summary>
/// 变参
/// </summary>
/// <param name="fmt">格式化</param>
/// <param name="args">变参</param>
/// <returns>char * 需要free</returns>
char *_format_va(const char *fmt, va_list args);
/// <summary>
/// 变参
/// </summary>
/// <param name="fmt">格式化</param>
/// <param name="...">变参</param>
/// <returns>char * 需要free</returns>
char *format_va(const char *fmt, ...);
/// <summary>
/// 用密码学安全随机数（CSPRNG）填充缓冲区。
/// 各平台实现：Windows=BCryptGenRandom，Darwin/BSD=arc4random_buf，
/// Linux=getrandom syscall，其余 Unix=/dev/urandom。
/// </summary>
/// <param name="buf">目标缓冲区</param>
/// <param name="len">填充字节数</param>
/// <returns>ERR_OK 成功，ERR_FAILED 失败</returns>
int32_t csprng_rand(void *buf, size_t len);

#endif//UTILS_H_
