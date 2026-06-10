#ifndef UTILS_H_
#define UTILS_H_

#include "base/macro.h"

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
/// 获取Content-Type
/// </summary>
/// <param name="extension">扩展名字.xx</param>
/// <returns>Content-Type</returns>
const char *contenttype(const char *extension);
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
/// timeofday
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
/// 字符串转时间戳
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
/// 安全填充定长字符串缓冲：src 为 NULL 时 dst 写空串；src 长度超 dstsz-1 时截断；
/// 始终保证 dst[dstsz-1]='\0'。dstsz 须 大于等于 1。
/// </summary>
/// <param name="dst">目标缓冲，dstsz 字节</param>
/// <param name="dstsz">目标缓冲总字节数（含末尾终止符）</param>
/// <param name="src">源字符串，可为 NULL</param>
void safe_fill_str(char *dst, size_t dstsz, const char *src);
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
/// 跳过空字节
/// </summary>
/// <param name="ptr">源字符</param>
/// <param name="plens">源字符长度</param>
/// <returns>void *, NULL全为空</returns>
void *skipempty(const void *ptr, size_t plens);
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
#define HEX_ENSIZE(s) (s * 2 + 1)
/// <summary>
/// 转16进制
/// </summary>
/// <param name="buf">要转的数据</param>
/// <param name="len">数据长度</param>
/// <param name="out">转换后的数据,长度:HEX_ENSIZE</param>
/// <returns>char *</returns>
char *tohex(const void *buf, size_t len, char *out);
/// <summary>
/// 拆分
/// </summary>
/// <param name="ptr">要拆分的数据</param>
/// <param name="plens">数据长度</param>
/// <param name="sep">拆分标记</param>
/// <param name="seplens">拆分标记长度</param>
/// <param name="n">拆分后的长度</param>
/// <returns>buf_ctx *, 需要free</returns>
struct buf_ctx *split(const void *ptr, size_t plens, const void *sep, size_t seplens, size_t *n);
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
/// 安全清零缓冲区。与 ZERO/memset 不同，保证写入不被编译器优化掉，
/// 适用于密钥、密码、PBKDF2 中间值等使用后须立即抹除的敏感缓冲。
/// 实现使用 volatile 指针 + GCC/Clang 编译器屏障防 dead-store elimination 与 LTO 内联消除。
/// </summary>
/// <param name="buf">目标缓冲区（NULL 时直接返回）</param>
/// <param name="len">字节数（0 时直接返回）</param>
void secure_zero(void *buf, size_t len);
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
