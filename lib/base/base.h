#ifndef BASE_H_
#define BASE_H_

#include "base/os.h"
#include "base/err.h"

#define HEX_ENSIZE(s) ((s) * 2 + 1) //tohex 输出缓冲长度：每字节两个十六进制字符 + 结尾 '\0'
typedef void *(*chr_func)(const void *, int32_t, size_t); //字符查找函数类型（类似 memchr）
typedef int32_t(*cmp_func)(const void *, const void *, size_t); //内存比较函数类型（类似 memcmp）

/// <summary>
/// 安全清零缓冲区。与 ZERO/memset 不同，保证写入不被编译器优化掉（含 LTO），
/// 适用于密钥、密码、PBKDF2 中间值等使用后须立即抹除的敏感缓冲。
/// </summary>
/// <param name="buf">目标缓冲区（NULL 时直接返回）</param>
/// <param name="len">字节数（0 时直接返回）</param>
void secure_zero(void *buf, size_t len);
/// <summary>
/// 查找字符，不区分大小写。大小写只按 ASCII 折叠('A'..'Z')，0x80 以上的字节原样比，不随 locale 变
/// </summary>
/// <param name="ptr">源字符</param>
/// <param name="val">需要查找的字符</param>
/// <param name="maxlen">最多搜索长度</param>
/// <returns>void * 字符出现的指针, NULL无</returns>
void *memichr(const void *ptr, int32_t val, size_t maxlen);
/// <summary>
/// 不区分大小写的内存比较，各平台同一份实现。折叠规则同 memichr；按长度比，内嵌 NUL 照常往后比
/// </summary>
/// <param name="ptr1">第一块内存指针</param>
/// <param name="ptr2">第二块内存指针</param>
/// <param name="lens">比较字节数</param>
/// <returns>0 相等；1 ptr1 大；-1 ptr1 小。大小按折叠后的无符号字节值比</returns>
int32_t memcasecmp(const void *ptr1, const void *ptr2, size_t lens);
/// <summary>
/// 不区分大小写的 C 串比较，折叠规则同 memichr。一般经 STRICMP 宏调用
/// </summary>
/// <param name="s1">第一个串，须以 NUL 结尾</param>
/// <param name="s2">第二个串，须以 NUL 结尾</param>
/// <returns>0 相等；1 s1 大；-1 s1 小。大小按折叠后的无符号字节值比，短串是长串前缀时短串小</returns>
int32_t strcasecmp_s(const char *s1, const char *s2);
/// <summary>
/// 同 strcasecmp_s，但最多比前 n 个字节，遇 NUL 提前结束。一般经 STRNCMP 宏调用
/// </summary>
/// <param name="s1">第一个串</param>
/// <param name="s2">第二个串</param>
/// <param name="n">最多比较的字节数</param>
/// <returns>同 strcasecmp_s；n 为 0 返回 0</returns>
int32_t strncasecmp_s(const char *s1, const char *s2, size_t n);
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
        *cmp = memcasecmp;
    }
}
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
/// 恒定时间内存比较，防止时序攻击。
/// 无论差异位置在哪，均遍历全部字节后返回，执行时间与内容无关。
/// </summary>
/// <param name="a">缓冲区 a</param>
/// <param name="b">缓冲区 b</param>
/// <param name="len">比较长度（字节）</param>
/// <returns>相等返回 0，不相等返回非 0</returns>
int32_t ct_memcmp(const void *a, const void *b, size_t len);
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
/// 把 (指针, 长度) 的字节段复制进定长栈缓冲并补 NUL。协议层把对端给的定长切片转成 C 串时用，
/// 这类地方是不可信字节进固定缓冲的唯一屏障，散着写容易各自漏一个 -1。
/// **默认用 strict 非 0**：截断只在这段字节纯给人看、没有任何逻辑解析它时才成立
/// </summary>
/// <param name="data">源字节段(可非 NUL 结尾)</param>
/// <param name="lens">源字节数</param>
/// <param name="dst">目标缓冲</param>
/// <param name="cap">目标缓冲总字节数(含 NUL)</param>
/// <param name="strict">非 0 装不下即返 ERR_FAILED；0 则截断</param>
/// <returns>ERR_OK 成功；ERR_FAILED：cap 为 0，或 strict 且装不下</returns>
static inline int32_t copy_bounded(const void *data, size_t lens, char *dst, size_t cap, int32_t strict) {
    if (0 == cap) {
        return ERR_FAILED;
    }
    size_t cplen = lens;
    if (lens >= cap) {
        if (0 != strict) {
            return ERR_FAILED;
        }
        cplen = cap - 1;
    }
    if (cplen > 0) {
        memcpy(dst, data, cplen);
    }
    dst[cplen] = '\0';
    return ERR_OK;
}
/// <summary>
/// 安全填充定长字符串缓冲：src 为 NULL 时 dst 写空串；成功时保证 dst 以 '\0' 结尾。
/// 装不下时不截断——截断的值拿去用是静默出错；报错文案由调用方判返回值后自己打
/// </summary>
/// <param name="dst">目标缓冲，dstsz 字节</param>
/// <param name="dstsz">目标缓冲总字节数（含末尾终止符）</param>
/// <param name="src">源字符串，可为 NULL</param>
/// <returns>ERR_OK 成功；ERR_FAILED dstsz 为 0 或 src 装不下（此时 dst 未被改动）</returns>
static inline int32_t safe_fill_str(char *dst, size_t dstsz, const char *src) {
    if (0 == dstsz) {
        return ERR_FAILED;
    }
    if (NULL == src) {
        dst[0] = '\0';
        return ERR_OK;
    }
    return copy_bounded(src, strlen(src), dst, dstsz, 1);
}
/// <summary>
/// 复制 src 的 lens 字节为新分配的 NUL 结尾字符串；按定长字节复制，不依赖 src 含 NUL。
/// </summary>
/// <param name="src">源缓冲</param>
/// <param name="lens">复制字节数</param>
/// <returns>新分配的 NUL 结尾字符串，调用方负责 FREE；分配失败时底层 _malloc 终止进程</returns>
char *dup_zero(const void *src, size_t lens);
/// <summary>
/// 按长度解析十进制无符号整数：只认数字，不接受前导空白与正负号，不要求 NUL 结尾。
/// 协议层拿到的普遍是切片，长度之外的字节不属于本值
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
/// 把 SNPRINTF 的返回值收敛成实际写入的字节数(不含结尾 NUL)。截断时它返回的是
/// "本应写入的长度"而非实际写入,凡把返回值直接当长度用的地方都要过本函数,
/// 否则会越过缓冲末尾把相邻字节一起读走
/// </summary>
/// <param name="rtn">SNPRINTF 的原始返回值</param>
/// <param name="bufsize">目标缓冲总字节数</param>
/// <returns>实际写入字节数;rtn 为负或 bufsize 为 0 返回 0,截断时返回 bufsize - 1</returns>
static inline size_t snprintf_lens(int32_t rtn, size_t bufsize) {
    if (rtn < 0
        || 0 == bufsize) {
        return 0;
    }
    return ((size_t)rtn < bufsize) ? (size_t)rtn : bufsize - 1;
}
/// <summary>
/// 从路径中提取文件名，按平台的 PATH_SEPARATOR 切
/// </summary>
/// <param name="file">路径，须非 NULL</param>
/// <returns>指向 file 内部最后一个分隔符之后的位置；没有分隔符时即 file 本身</returns>
const char *_filename(const char *file);
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
/// 单个十六进制字符转数值，大小写均可。非法字符自己认得出来，调用方不必先 isxdigit
/// </summary>
/// <param name="c">待转换字符</param>
/// <returns>0-15；ERR_FAILED 不是十六进制字符</returns>
int32_t fromhex(char c);
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
#endif//OS_WIN OS_DARWIN OS_AIX
/// <summary>
/// 将 n 向上取整到最近的 2 的幂（uint32_t 范围）。
/// </summary>
/// <param name="n">输入值</param>
/// <returns>最接近且不小于 n 的 2 的幂；n 已是 2 的幂时原值返回，0 返回 0；
/// 大于 0x80000000u 时 ASSERTAB 中止（uint32 无法表示更大的 2 的幂）</returns>
uint32_t pow2_ceil(uint32_t n);
/// <summary>
/// 填充timespec
/// </summary>
/// <param name="timeout">struct timespec</param>
/// <param name="ms">毫秒</param>
void fill_timespec(struct timespec *timeout, uint32_t ms);
/// <summary>
/// 输出一条日志，低于当前日志级别时直接忽略。实现在 utils/log.c，声明放这一层
/// 是为了让 macro.h 的 LOG 宏不必反向依赖 utils/log.h。
/// 由此带来一条链接约束：macro.h 的 LOG_* 与 ASSERTAB 展开后都指向 utils/log.c，凡用到它们的
/// target 都必须链上 lib/utils，只链 base + containers 会在某处无关的断言上报未定义符号
/// </summary>
/// <param name="lv">日志级别，参见 log_level</param>
/// <param name="fmt">格式化字符串</param>
/// <param name="...">变参</param>
void slog(int32_t lv, const char *fmt, ...);
/// <summary>
/// ASSERTAB 专用：排空日志队列后把 abort 原因写进日志文件；无日志文件时不重复写原因行
/// （ASSERTAB 已打到 stderr），只把缓冲刷出去。声明放这一层的理由同 slog。
/// 尽力而为——不保证落盘，但每一步都有上限，不会把崩溃卡成挂起。可并发调用、可重复调用，
/// 各种情形下分别做什么见实现里的分支注释
/// </summary>
/// <param name="file">断言所在文件，由 ASSERTAB 传 __FILENAME__</param>
/// <param name="func">断言所在函数</param>
/// <param name="line">断言所在行</param>
/// <param name="msg">断言的错误描述，非空</param>
void log_abort(const char *file, const char *func, int32_t line, const char *msg);

#endif//BASE_H_
