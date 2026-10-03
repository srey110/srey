#ifndef BYTES_H_
#define BYTES_H_

#include "base/os.h"
#include "base/err.h"
#include "base/bits.h"

// 字节串，内存的查找、比较、裁剪、复制与格式化

/// <summary>
/// 查找字符，不区分大小写。大小写只按 ASCII 折叠('A'..'Z')，0x80 以上的字节原样比，不随 locale 变
/// </summary>
/// <param name="ptr">源字符</param>
/// <param name="val">需要查找的字符</param>
/// <param name="maxlen">最多搜索长度</param>
/// <returns>void * 字符出现的指针, NULL无</returns>
void *memichr(const void *ptr, int32_t val, size_t maxlen);
/// <summary>
/// 不区分大小写的内存比较，各平台同一份实现。折叠规则同 memichr；按长度比，内嵌 NUL 照常往后比。
/// 逐字节比，遇到第一个不同的字节就返回、不再往后读：C 串之间比较时 lens 可取一边的 strlen + 1(连 '\0' 一起比，
/// 排序同 strcasecmp)，另一边更短时在它的 '\0' 处先对不上就停，不会读过去
/// </summary>
/// <param name="ptr1">第一块内存指针</param>
/// <param name="ptr2">第二块内存指针</param>
/// <param name="lens">比较字节数</param>
/// <returns>0 相等；1 ptr1 大；-1 ptr1 小。大小按折叠后的无符号字节值比</returns>
int32_t memcasecmp(const void *ptr1, const void *ptr2, size_t lens);
// memcspn 里 what 超过 4 个字符的那支(查表逐字节比)，由 memcspn 转调；wlens 须非 0
size_t _memcspn(const void *p, size_t lens, const char *what, size_t wlens);
// memcspn 总长不足 16 字节那支(首尾重叠读拼成整数比，不逐字节循环)，由 memcspn 转调；只在有向量指令的平台编译，wlens 须在 1~4
size_t _memcspn_short(const void *p, size_t lens, const char *what, size_t wlens);
/// <summary>
/// 数 p 开头有多少个字节都不在 what[0..wlens) 里（集合里任意一个字节），语义同 strcspn，但 p 按长度算、不要求 '\0' 结尾，what 里也可以含 '\0'。
/// wlens 不超过 4 时每轮比 16 字节，最后不足 16 字节的尾巴从末尾往前重叠再读一整块；总长不足 16 字节的首尾各读一次拼成 64 位整数比
/// (没有向量指令的平台每个字符 memchr 一次)；
/// 超过 4 转非内联的 _memcspn 查表逐字节比，只求正确。
/// what / wlens 传常量时内联后比较模板在编译期算好、逐字符比较展开，与手写的专用扫描同价
/// </summary>
/// <param name="p">待查字节段，须非 NULL，不要求 '\0' 结尾</param>
/// <param name="lens">p 的字节数</param>
/// <param name="what">要找的字符集合，按字节比，可含 '\0'</param>
/// <param name="wlens">what 的字节数</param>
/// <returns>第一个落在 what 里的字节的下标；一个都不在(含 wlens 为 0)返回 lens</returns>
static inline size_t memcspn(const void *p, size_t lens, const char *what, size_t wlens) {
    const unsigned char *start = (const unsigned char *)p;
    size_t k;
    if (0 == wlens) {
        return lens;
    }
    if (wlens > 4) {
        return _memcspn(p, lens, what, wlens);
    }
#if defined(SIMD_SSE2) || defined(SIMD_NEON)
    if (lens < 16) {
        return _memcspn_short(p, lens, what, wlens);
    }
#endif
#if defined(SIMD_SSE2)
    const unsigned char *s = start;
    const unsigned char *end = start + lens;
    __m128i v, m;
    uint32_t bits;
    // 每轮看 16 个字节。例：what 为 "\0\r\n"、p 指向 "Host: x\r\n..." 时，下标 7 的 \r 与下标 8 的 \n 都命中，
    // 第 3 步得到 bits = 0x180，最低的 1 在第 7 位，返回 7
    while (end - s >= 16) {
        // 第 1 步：一次读 16 字节进向量寄存器(loadu 的 u 表示地址不必 16 字节对齐)
        v = _mm_loadu_si128((const __m128i *)s);
        // 第 2 步：what 里每个字符造一个 16 格全是它的模板(set1_epi8)，拿这 16 字节和它逐格比(cmpeq_epi8)，相等那格置 0xFF、否则 0x00；
        // 各份结果按位或(or_si128)合成一份：某格是 what 里任意一个字符，那格就是 0xFF
        m = _mm_cmpeq_epi8(v, _mm_set1_epi8(what[0]));
        for (k = 1; k < wlens; k++) {
            m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8(what[k])));
        }
        // 第 3 步：16 格各取最高位拼成 16 位整数(movemask_epi8)，第 i 位为 1 表示第 i 个字节命中
        bits = (uint32_t)_mm_movemask_epi8(m);
        // 第 4 步：有命中就返回最靠前的那个(最低那个 1 的位置)；一个都没有就往后挪 16 字节接着找
        if (0 != bits) {
            return (size_t)(s + ctz32(bits) - start);
        }
        s += 16;
    }
    // 第 5 步：剩下不足 16 字节时，从 end-16 再读一整块，照第 1~3 步比一遍。这块前 16 - r 个字节(r 为剩下的字节数)
    // 主循环已经比过，把位掩码右移 16 - r 位丢掉它们，剩下的第 0 位正好对应 s。
    // 例：lens 为 20，主循环比完下标 0~15、剩 4 个，这次读下标 4~19，右移 12 位后只看下标 16~19
    if (s < end) {
        v = _mm_loadu_si128((const __m128i *)(end - 16));
        m = _mm_cmpeq_epi8(v, _mm_set1_epi8(what[0]));
        for (k = 1; k < wlens; k++) {
            m = _mm_or_si128(m, _mm_cmpeq_epi8(v, _mm_set1_epi8(what[k])));
        }
        bits = (uint32_t)_mm_movemask_epi8(m) >> (16 - (size_t)(end - s));
        return 0 != bits ? (size_t)(s + ctz32(bits) - start) : lens;
    }
    return lens;
#elif defined(SIMD_NEON)
    const unsigned char *s = start;
    const unsigned char *end = start + lens;
    uint8x16_t v, m;
    uint64_t bits;
    // 每轮看 16 个字节。例：what 为 "\0\r\n"、p 指向 "Host: x\r\n..." 时，下标 7 的 \r 与下标 8 的 \n 都命中，
    // 第 3 步得到 bits = 0xFF0000000，最低的 1 在第 28 位、除以 4 得 7，返回 7
    while (end - s >= 16) {
        // 第 1 步：一次读 16 字节进向量寄存器(vld1q_u8，不要求对齐)
        v = vld1q_u8(s);
        // 第 2 步：what 里每个字符造一个 16 格全是它的模板(vdupq_n_u8)，拿这 16 字节和它逐格比(vceqq_u8)，相等那格置 0xFF、否则 0x00；
        // 各份结果按位或(vorrq_u8)合成一份：某格是 what 里任意一个字符，那格就是 0xFF
        m = vceqq_u8(v, vdupq_n_u8((uint8_t)what[0]));
        for (k = 1; k < wlens; k++) {
            m = vorrq_u8(m, vceqq_u8(v, vdupq_n_u8((uint8_t)what[k])));
        }
        // 第 3 步：NEON 没有 movemask，换个办法压成整数：16 格两两看成 8 个 16 位数，各右移 4 位只留低 8 位(vshrn_n_u16)，
        // 每格的 0xFF / 0x00 就缩成 4 位的 F / 0，16 格正好拼成一个 64 位整数(vget_lane_u64 取出)，第 i 个字节占第 4i~4i+3 位；
        // vreinterpretq_u16_u8 / vreinterpret_u64_u8 只是换个类型看同一份数据，不产生指令
        bits = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
        // 第 4 步：有命中就返回最靠前的那个(最低那个 1 的位置除以 4)；一个都没有就往后挪 16 字节接着找
        if (0 != bits) {
            return (size_t)(s + (ctz64(bits) >> 2) - start);
        }
        s += 16;
    }
    // 第 5 步：同 SSE2 那支，剩下不足 16 字节时从 end-16 重叠再读一整块；这里每个字节占 4 位，
    // 所以右移 4 * (16 - r) 位丢掉主循环已经比过的那部分，剩下的最低 4 位正好对应 s
    if (s < end) {
        v = vld1q_u8(end - 16);
        m = vceqq_u8(v, vdupq_n_u8((uint8_t)what[0]));
        for (k = 1; k < wlens; k++) {
            m = vorrq_u8(m, vceqq_u8(v, vdupq_n_u8((uint8_t)what[k])));
        }
        bits = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0) >> (4 * (16 - (size_t)(end - s)));
        return 0 != bits ? (size_t)(s + (ctz64(bits) >> 2) - start) : lens;
    }
    return lens;
#else
    // 没有向量指令的平台：每个字符 memchr 一次，后一次只查到前面已找到的最靠前位置为止
    const unsigned char *q;
    for (k = 0; k < wlens; k++) {
        q = (const unsigned char *)memchr(start, what[k], lens);
        if (NULL != q) {
            lens = (size_t)(q - start);
        }
    }
    return lens;
#endif
}
// 区分大小写的逐字节查找(memchr 找首字节)，memstr 向量主循环剩下的起点(没有向量指令的平台是整段)转它；
// 也可单独用：ptr / what 须非 NULL，wlen 为 0 或大于 plens 返回 NULL
void *_memstr(const void *ptr, size_t plens, const void *what, size_t wlen);
// 不区分大小写的查找，memstr(1, ...) 转它；也可单独用，参数约定同 _memstr
void *_memistr(const void *ptr, size_t plens, const void *what, size_t wlen);
/// <summary>
/// 内存查找：在 ptr 里找 what 第一次出现的位置。区分大小写时每轮用向量指令一次筛 16 个起点(首字节、末字节都对上的才算候选)，
/// 候选再比中间；剩下不足 16 个起点与没有向量指令的平台转非内联的 _memstr(memchr 找首字节)。不区分大小写转 _memistr。
/// what / wlen 传常量时内联后中间那段比较折成定长比较
/// </summary>
/// <param name="ncs">0 区分大小写，非 0 不区分(只按 ASCII 折叠，同 memichr)</param>
/// <param name="ptr">源数据</param>
/// <param name="plens">源数据长度</param>
/// <param name="what">要查找的字节串</param>
/// <param name="wlen">what 长度</param>
/// <returns>第一次出现处的指针；没找到，或任一指针为 NULL、任一长度为 0、wlen 大于 plens，返回 NULL</returns>
static inline void *memstr(int32_t ncs, const void *ptr, size_t plens, const void *what, size_t wlen) {
    const char *p = (const char *)ptr;
    const char *w = (const char *)what;
    size_t i = 0;
    if (NULL == ptr
        || NULL == what
        || 0 == plens
        || 0 == wlen
        || wlen > plens) {
        return NULL;
    }
    //不区分大小写
    if (0 != ncs) {
        return _memistr(ptr, plens, what, wlen);
    }
    //区分大小写，并且目标1字节
    if (1 == wlen) {
        return (void *)memchr(ptr, w[0], plens);
    }
#if defined(SIMD_SSE2)
    size_t nstart = plens - wlen + 1;// 可能的起点个数
    __m128i vf, vl, m;
    uint32_t bits, k;
    // 两个模板：16 格全是 what 首字节、全是 what 末字节(set1_epi8，同 memcspn)
    vf = _mm_set1_epi8(w[0]);
    vl = _mm_set1_epi8(w[wlen - 1]);
    // 每轮看 i ~ i+15 这 16 个起点；第二份要读到 p[i + 15 + wlen - 1]，所以要求 i + 16 <= 起点个数。
    // 例：找 "\r\n\r\n"、p 为 "k: v\r\nx: y\r\n\r\nbody..." 时 \r 在下标 4、10、12，往后第 3 字节是 \n 的只有 10，bits = 0x400
    for (; i + 16 <= nstart; i += 16) {
        // 第 1 步：读两份 16 字节，一份从 p+i 起，一份从 p+i+wlen-1 起(正好是每个起点的末字节)，前一份和首字节比、后一份和末字节比
        // (loadu / cmpeq_epi8 同 memcspn)，两个结果按位与(and_si128)：第 j 格为 0xFF 表示从 i+j 起首末两字节都对上
        m = _mm_and_si128(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i)), vf),
                          _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i + wlen - 1)), vl));
        // 第 2 步：16 格各取最高位拼成 16 位整数(movemask_epi8，同 memcspn)，第 j 位为 1 表示 i+j 是候选
        bits = (uint32_t)_mm_movemask_epi8(m);
        // 第 3 步：从最靠前的候选起比中间那段(wlen 为 2 时没有中间)，对上就是答案；
        // 对不上就用 bits &= bits - 1 只清掉最低那个 1，接着看下一个候选
        while (0 != bits) {
            k = ctz32(bits);
            if (wlen <= 2
                || 0 == memcmp(p + i + k + 1, w + 1, wlen - 2)) {
                return (void *)(p + i + k);
            }
            bits &= bits - 1;
        }
    }
#elif defined(SIMD_NEON)
    size_t nstart = plens - wlen + 1;// 可能的起点个数
    uint8x16_t vf, vl, m;
    uint64_t bits;
    uint32_t k;
    // 两个模板：16 格全是 what 首字节、全是 what 末字节(vdupq_n_u8，同 memcspn)
    vf = vdupq_n_u8((uint8_t)w[0]);
    vl = vdupq_n_u8((uint8_t)w[wlen - 1]);
    // 每轮看 i ~ i+15 这 16 个起点，读法与例子同 SSE2 那支(那里 bits = 0x400，这里是第 43 位)
    for (; i + 16 <= nstart; i += 16) {
        // 第 1 步：读两份 16 字节分别和首、末字节比(vld1q_u8 / vceqq_u8 同 memcspn)，两个结果按位与(vandq_u8)：
        // 第 j 格为 0xFF 表示从 i+j 起首末两字节都对上
        m = vandq_u8(vceqq_u8(vld1q_u8((const uint8_t *)p + i), vf),
                     vceqq_u8(vld1q_u8((const uint8_t *)p + i + wlen - 1), vl));
        // 第 2 步：压成 64 位整数，第 j 个起点占第 4j~4j+3 位(vshrn_n_u16 那套，同 memcspn)；
        // 再和 0x8888... 与一下，每个起点只留最高那 1 位，第 3 步就能像 SSE2 那样一次清一个
        bits = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0) & 0x8888888888888888ull;
        // 第 3 步：从最靠前的候选起(最低那个 1 的位置除以 4)比中间那段，对上就是答案；对不上 bits &= bits - 1 清掉它接着看
        while (0 != bits) {
            k = ctz64(bits) >> 2;
            if (wlen <= 2
                || 0 == memcmp(p + i + k + 1, w + 1, wlen - 2)) {
                return (void *)(p + i + k);
            }
            bits &= bits - 1;
        }
    }
#endif
    // 剩下的起点(不足 16 个，或没有向量指令)交给非内联的 _memstr，这段循环不在每个调用点各内联一份
    return _memstr(p + i, plens - i, what, wlen);
}
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
/// 从路径中提取文件名，按平台的 PATH_SEPARATOR 切
/// </summary>
/// <param name="file">路径，须非 NULL</param>
/// <returns>指向 file 内部最后一个分隔符之后的位置；没有分隔符时即 file 本身</returns>
const char *_filename(const char *file);
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
/// 判断是否为合法 RFC 7230 token（全部字符为 tchar）。
/// 头名、WebSocket 子协议名等都按此校验：只挡 NUL/CRLF 不够，
/// 键里混进 ':' 或 ' ' 同样会被对端拆成两个字段（走私），故按整个 tchar 集合校验
/// </summary>
/// <param name="data">源数据(可非 NUL 结尾)</param>
/// <param name="lens">源数据长度</param>
/// <returns>是合法 token 返回 1，否则 0（空串不是 token）</returns>
int32_t is_token(const char *data, size_t lens);
/// <summary>
/// 从 data 起数连续的 token 字符(字符集同 is_token)，遇到第一个非 token 字节即停
/// </summary>
/// <param name="data">源数据，lens 为 0 时可为 NULL</param>
/// <param name="lens">最多数这么多字节</param>
/// <returns>开头 token 字符的个数，0 表示首字节就不是 token 字符</returns>
size_t token_span(const char *data, size_t lens);
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

#endif//BYTES_H_
