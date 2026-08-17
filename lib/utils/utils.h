#ifndef UTILS_H_
#define UTILS_H_

#include "base/macro.h"

#define HEX_ENSIZE(s) ((s) * 2 + 1) //tohex 输出缓冲长度：每字节两个十六进制字符 + 结尾 '\0'
typedef void *(*chr_func)(const void *, int32_t, size_t);   //字符查找函数类型（类似 memchr）
typedef int32_t(*cmp_func)(const void *, const void *, size_t); //内存比较函数类型（类似 memcmp）

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
/// 信号处理
/// 警告：cb 在信号处理上下文中被调用，POSIX 要求其内部只能使用 async-signal-safe 函数。
/// 调用 LOG_INFO、mutex_lock、malloc 等均属未定义行为，可能导致死锁或堆损坏。
/// 安全做法：在 cb 中仅设置 volatile sig_atomic_t 标志，由主循环轮询后再处理。
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
/// <param name="file">路径</param>
/// <returns>ERR_OK 文件</returns>
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
/// 读取文件全部
/// </summary>
/// <param name="file">路径</param>
/// <param name="lens">文件大小</param>
/// <returns>文件内容</returns>
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
/// <returns>分</returns>
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
/// <returns>ERR_OK 成功，ERR_FAILED 失败</returns>
int32_t mstostr(uint64_t ms, const char *fmt, char time[TIME_LENS]);
/// <summary>
/// 字符串转时间戳。
/// fmt 里的 %z / %Z 只吃掉时区文本，偏移量不生效：本仓库从未定义 TM_GMTOFF，
/// strptime 解析出的偏移进不了 struct tm，最终一律按本地时间 mktime。
/// 所以带时区的串解析结果会差“串里的偏移 − 本地偏移”秒，需要按时区换算的调用方得自己取偏移补回去
/// </summary>
/// <param name="time">时间字符串</param>
/// <param name="fmt">格式化</param>
/// <returns>时间戳</returns>
uint64_t strtots(const char *time, const char *fmt);
/// <summary>
/// 填充timespec
/// </summary>
/// <param name="timeout">struct timespec</param>
/// <param name="ms">毫秒</param>
void fill_timespec(struct timespec *timeout, uint32_t ms);
/// <summary>
/// hash
/// </summary>
/// <param name="buf">要计算的数据</param>
/// <param name="len">数据长度</param>
/// <returns>hash</returns>
uint64_t hash(const char *buf, size_t len);
/// <summary>
/// 是否 HTTP/1.1 的 OWS（RFC 7230 §3.2.3：仅 SP 与 HTAB）。
/// trim_left / trim_right / trim 剔除的就是这两个字符；任何"须拒绝 OWS"的解析器
/// （如 http.c 的 obs-fold 续行、字段名与冒号间空白）必须用本判定，不要各自手写
/// ' ' / '\t' 比较——漏一个字符即多一条走私绕过路径
/// </summary>
/// <param name="ch">待判字符</param>
/// <returns>是 OWS 返回 1，否则 0</returns>
static inline int32_t is_ows(char ch) {
    return ' ' == ch || '\t' == ch;
}
/// <summary>
/// 判断是否为合法 RFC 7230 token（全部字符为 tchar）。头名、WebSocket 子协议名等都按此校验；
/// 空串不是 token，返 0。
/// 组头名时只挡 NUL/CRLF 不够：键里混进 ':' 或 ' ' 同样会让对端把一行拆成两个字段，
/// 攻击者借此就能塞进一个自选的头值，故按整个 tchar 集合校验
/// </summary>
/// <param name="data">源数据(可非 NUL 结尾)</param>
/// <param name="lens">源数据长度</param>
/// <returns>是合法 token 返回 1，否则 0</returns>
int32_t is_token(const char *data, size_t lens);
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
/// 查找字符，不区分大小写
/// </summary>
/// <param name="ptr">源字符</param>
/// <param name="val">需要查找的字符</param>
/// <param name="maxlen">最多搜索长度</param>
/// <returns>void * 字符出现的指针, NULL无</returns>
void *memichr(const void *ptr, int32_t val, size_t maxlen);
/// <summary>
/// 按 ncs 选大小写敏感(0)或不敏感的查找/比较组合。凡按 ncs 分流的搜索入口都用它,
/// 别各自写 if/else —— 分支写反只表现为搜索结果多一条或少一条, 不会崩, 极难发现
/// </summary>
/// <param name="ncs">0 区分大小写, 非 0 不区分</param>
/// <param name="chr">回填查找函数</param>
/// <param name="cmp">回填比较函数</param>
static inline void mem_funcs_pick(int32_t ncs, chr_func *chr, cmp_func *cmp) {
    if (0 == ncs) {
        *chr = memchr;
        *cmp = memcmp;
    } else {
        *chr = memichr;
        *cmp = _memicmp;
    }
}
/// <summary>
/// 安全填充定长字符串缓冲：src 为 NULL 时 dst 写空串；成功时保证 dst 以 '\0' 结尾。
/// 装不下（strlen(src) >= dstsz）时 dst 一个字节都不写、保持原样，返回 ERR_FAILED——
/// 截断后的值拿去用往往是静默出错，调用方从 dst 上看不出发生过什么。
/// 报错文案由调用方在判返回值后自己打，只有它知道这是哪个字段
/// </summary>
/// <param name="dst">目标缓冲，dstsz 字节</param>
/// <param name="dstsz">目标缓冲总字节数（含末尾终止符）</param>
/// <param name="src">源字符串，可为 NULL</param>
/// <returns>ERR_OK 成功；ERR_FAILED dstsz 为 0 或 src 装不下（此时 dst 未被改动）</returns>
int32_t safe_fill_str(char *dst, size_t dstsz, const char *src);
/// <summary>
/// 把 (指针, 长度) 的字节段复制进定长栈缓冲并补 NUL。协议层把对端给的定长切片转成 C 串时用，
/// 这类地方是不可信字节进固定缓冲的唯一屏障，散着写容易各自漏一个 -1。
/// strict 非 0：装不下即失败且不写 dst；strict 为 0：截断到 cap-1。
/// **默认用 strict 非 0**。截断只在"这段字节纯粹给人看、没有任何逻辑解析它"时才成立
/// （全仓仅 _mpack_err 的服务端错误文本一处）；只要有人 parse 它，截出来的值就是静默出错，
/// 调用方从 dst 上还看不出发生过什么——safe_fill_str 当初删掉截断语义就是这个原因
/// <param name="data">源字节段(可非 NUL 结尾)</param>
/// <param name="lens">源字节数</param>
/// <param name="dst">目标缓冲</param>
/// <param name="cap">目标缓冲总字节数(含 NUL)</param>
/// <param name="strict">非 0 装不下即返 ERR_FAILED；0 则截断</param>
/// <returns>ERR_OK 成功；ERR_FAILED：cap 为 0，或 strict 且装不下</returns>
int32_t copy_bounded(const void *data, size_t lens, char *dst, size_t cap, int32_t strict);
/// <summary>
/// 复制 src 的 lens 字节为新分配的 NUL 结尾字符串，返回堆缓冲，调用方负责 FREE；
/// 按定长字节复制，不依赖 src 含 NUL；分配失败时底层 _malloc 终止进程。
/// </summary>
/// <param name="src">源缓冲</param>
/// <param name="lens">复制字节数</param>
/// <returns>新分配的 NUL 结尾字符串</returns>
char *dup_zero(const void *src, size_t lens);
/// <summary>
/// 内存查找
/// </summary>
/// <param name="ncs">0 区分大小写</param>
/// <param name="ptr">源字符</param>
/// <param name="plens">源字符长度</param>
/// <param name="what">要查找的字符串</param>
/// <param name="wlen">what长度</param>
/// <returns>void * 字符出现的指针, NULL无</returns>
void *memstr(int32_t ncs, const void *ptr, size_t plens, const void *what, size_t wlen);
/// <summary>
/// 剔除左端空字节(SP/HTAB)，不改动源数据、不写 '\0'
/// </summary>
/// <param name="data">源数据(可非 NUL 结尾)</param>
/// <param name="dlens">源数据长度</param>
/// <param name="lens">输出：剔除后长度；返回 NULL 时写 0</param>
/// <returns>指向剔除后首字节(仍在 data 内)，全为空字节则返回 NULL</returns>
char *trim_left(char *data, size_t dlens, size_t *lens);
/// <summary>
/// 剔除右端空字节(SP/HTAB)，不改动源数据、不写 '\0'
/// </summary>
/// <param name="data">源数据(可非 NUL 结尾)</param>
/// <param name="dlens">源数据长度</param>
/// <param name="lens">输出：剔除后长度；返回 NULL 时写 0</param>
/// <returns>data 本身(起点不变)，全为空字节则返回 NULL</returns>
char *trim_right(char *data, size_t dlens, size_t *lens);
/// <summary>
/// 剔除两端空字节(SP/HTAB)，不改动源数据、不写 '\0'；等价于 trim_left 后接 trim_right
/// </summary>
/// <param name="data">源数据(可非 NUL 结尾)</param>
/// <param name="dlens">源数据长度</param>
/// <param name="lens">输出：剔除后长度；返回 NULL 时写 0</param>
/// <returns>指向剔除后首字节(仍在 data 内)，全为空字节则返回 NULL</returns>
char *trim(char *data, size_t dlens, size_t *lens);
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
/// 按长度解析十进制无符号整数。不用 strtoull/strtoul 是因为它们要求 NUL 结尾、
/// 且会静默接受前导空白与 '+' / '-'（负号还会回绕成巨大的正数），
/// 而本函数的调用方拿到的普遍是切片：长度之外的字节要么不属于本值，要么根本不存在。
/// </summary>
/// <param name="str">数值起始位置，不要求 NUL 结尾</param>
/// <param name="lens">参与解析的字节数，须 大于 0</param>
/// <param name="max">允许的最大值，超出即失败（可直接传目标类型的 *_MAX 折叠上界检查）</param>
/// <param name="out">输出：解析结果；失败时不写</param>
/// <returns>ERR_OK 成功；空串 / 含任一非数字字符 / 溢出 / 超过 max 均返回 ERR_FAILED</returns>
int32_t str2u64(const char *str, size_t lens, uint64_t max, uint64_t *out);
/// <summary>
/// 转大写
/// </summary>
/// <param name="str">源字符</param>
/// <returns>char *</returns>
char *strupper(char *str);
/// <summary>
/// 转小写
/// </summary>
/// <param name="str">源字符</param>
/// <returns>char *</returns>
char *strlower(char *str);
/// <summary>
/// 反转
/// </summary>
/// <param name="str">源字符</param>
/// <returns>char *</returns>
char* strreverse(char* str);
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
/// 转16进制
/// </summary>
/// <param name="buf">要转的数据</param>
/// <param name="len">数据长度</param>
/// <param name="out">转换后的数据,长度:HEX_ENSIZE</param>
/// <param name="lower">0 输出大写；非 0 输出小写</param>
/// <returns>char *</returns>
char *tohex(const void *buf, size_t len, char *out, int32_t lower);
/// <summary>
/// 拆分
/// </summary>
/// <param name="ptr">要拆分的数据</param>
/// <param name="plens">数据长度</param>
/// <param name="sep">拆分标记</param>
/// <param name="seplens">拆分标记长度</param>
/// <param name="n">输出:拆分后的段数;任何路径都会写,返回 NULL 时置 0</param>
/// <returns>buf_ctx *, 需要free;ptr 为 NULL 或 plens 为 0 时返回 NULL</returns>
struct buf_ctx *split(const void *ptr, size_t plens, const void *sep, size_t seplens, size_t *n);
/// <summary>
/// 按单字节 sep 就地拆分到调用方栈数组,不堆分配;标准切分保留空段(连续/尾随 sep 产生 len==0 段,段数 = sep 数 + 1)。
/// 仅记录段 (data,lens),不复制,data 指向 ptr 内部
/// </summary>
/// <param name="ptr">待拆分缓冲(只读取,不修改)</param>
/// <param name="plens">ptr 字节长度</param>
/// <param name="sep">单字节分隔符</param>
/// <param name="segs">输出段数组,调用方分配</param>
/// <param name="cap">segs 容量</param>
/// <returns>段数量;超过 cap 返回 ERR_FAILED</returns>
int32_t split2(char *ptr, size_t plens, uint8_t sep, struct buf_ctx *segs, int32_t cap);
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
/// 大小端判断
/// </summary>
/// <returns>1 小端, 0 大端</returns>
int32_t is_little(void);
/// <summary>
/// 将 n 向上取整到最近的 2 的幂（uint32_t 范围）。
/// n 已是 2 的幂时原值返回；0 返回 0；大于 0x80000000u 时 ASSERTAB 中止（uint32 无法表示更大的 2 的幂）。
/// </summary>
/// <param name="n">输入值</param>
/// <returns>最接近且不小于 n 的 2 的幂</returns>
uint32_t pow2_ceil(uint32_t n);
/// <summary>
/// 数字转 char*
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="val">数字</param>
/// <param name="size">字节数</param>
/// <param name="islittle">是否为小端</param>
void pack_integer(char *buf, uint64_t val, int32_t size, int32_t islittle);
/// <summary>
/// char* 转数字
/// </summary>
/// <param name="buf">要转换的buffer</param>
/// <param name="size">字节数</param>
/// <param name="islittle">是否为小端</param>
/// <param name="issigned">是否有符号</param>
/// <returns>数字</returns>
int64_t unpack_integer(const char *buf, int32_t size, int32_t islittle, int32_t issigned);
/// <summary>
/// float转 char*
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="val">值</param>
/// <param name="islittle">是否为小端</param>
void pack_float(char *buf, float val, int32_t islittle);
/// <summary>
/// char* 转float
/// </summary>
/// <param name="buf">要转换的buffer</param>
/// <param name="islittle">是否为小端</param>
/// <returns>float</returns>
float unpack_float(const char *buf, int32_t islittle);
/// <summary>
/// double转 char*
/// </summary>
/// <param name="buf">buffer</param>
/// <param name="val">值</param>
/// <param name="islittle">是否为小端</param>
void pack_double(char *buf, double val, int32_t islittle);
/// <summary>
/// char* 转double
/// </summary>
/// <param name="buf">要转换的buffer</param>
/// <param name="islittle">是否为小端</param>
/// <returns>double</returns>
double unpack_double(const char *buf, int32_t islittle);
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_AIX)
/// <summary>
/// 64 位网络字节序转主机字节序
/// </summary>
/// <param name="val">网络字节序值</param>
/// <returns>主机字节序值</returns>
uint64_t ntohll(uint64_t val);
/// <summary>
/// 64 位主机字节序转网络字节序
/// </summary>
/// <param name="val">主机字节序值</param>
/// <returns>网络字节序值</returns>
uint64_t htonll(uint64_t val);
#endif
/// <summary>
/// 恒定时间内存比较，防止时序攻击。
/// 无论差异位置在哪，均遍历全部字节后返回，执行时间与内容无关。
/// </summary>
/// <param name="a">缓冲区 a</param>
/// <param name="b">缓冲区 b</param>
/// <param name="len">比较长度（字节）</param>
/// <returns>相等返回 0，不相等返回非 0</returns>
int32_t ct_memcmp(const void *a, const void *b, size_t len);
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
