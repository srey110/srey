#ifndef STRCONV_H_
#define STRCONV_H_

#include "base/os.h"

// 字符串与数字、十六进制之间的转换
#define HEX_ENSIZE(s) ((s) * 2 + 1) //tohex 输出缓冲长度：每字节两个十六进制字符 + 结尾 '\0'
#define INT2STR_MAX 66 //u64tostr / i64tostr 输出缓冲长度：二进制 64 位 + 负号 + 结尾 '\0'

/// <summary>
/// 按长度解析十进制无符号整数：只认数字，不接受前导空白与正负号，不要求 NUL 结尾。
/// 协议层拿到的普遍是切片，长度之外的字节不属于本值
/// </summary>
/// <param name="str">数值起始位置，不要求 NUL 结尾</param>
/// <param name="lens">参与解析的字节数，须 大于 0</param>
/// <param name="max">允许的最大值，超出即失败（可直接传目标类型的 *_MAX 折叠上界检查）</param>
/// <param name="out">输出：解析结果；失败时不写</param>
/// <returns>ERR_OK 成功；空串 / 含任一非数字字符 / 溢出 / 超过 max 均返回 ERR_FAILED</returns>
int32_t strtou64(const char *str, size_t lens, uint64_t max, uint64_t *out);
/// <summary>
/// (指针, 长度) 的十进制整数文本转 int64，按符号拆开走 strtou64。
/// mysql / pgsql 的文本协议与 redis 的长度行共用，别再各写一份。
/// 只认 ['-']1*DIGIT：前导空白、'+' 都拒，redis 长度行靠这一点与对端切出同样的包边界
/// </summary>
/// <param name="data">源字节段(可非 NUL 结尾)</param>
/// <param name="lens">源字节数；0 视为失败</param>
/// <param name="val">输出：解析结果；返回 ERR_FAILED 时不写</param>
/// <returns>ERR_OK 成功；空串/只有负号/含非数字字符/超出 int64 量程返回 ERR_FAILED</returns>
int32_t strtoi64(const void *data, size_t lens, int64_t *val);
/// <summary>
/// 无符号整数按 base 进制转成文本写进 out：小写、无前导零，同 %llu / %llx，末尾补 '\0'
/// </summary>
/// <param name="out">输出缓冲，只写 位数 + 1(结尾 '\0') 个字节：任意进制开 INT2STR_MAX 即够，十进制 21 字节即够</param>
/// <param name="v">值</param>
/// <param name="base">进制，取值 [2, 16]，超界断言</param>
/// <returns>写入的字符数，不含结尾 '\0'</returns>
size_t u64tostr(char *out, uint64_t v, uint32_t base);
/// <summary>
/// 同 u64tostr，负数前面加 '-'
/// </summary>
/// <param name="out">输出缓冲，同 u64tostr，负数多写一个 '-'；十进制仍是 21 字节即够(INT64_MIN 为负号 + 19 位)</param>
/// <param name="v">值</param>
/// <param name="base">进制，取值 [2, 16]，超界断言</param>
/// <returns>写入的字符数(含负号)，不含结尾 '\0'</returns>
size_t i64tostr(char *out, int64_t v, uint32_t base);
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
/// 十进制浮点串的精确快路径：只收整段恰为 [-]数字[.数字][(e|E)[+-]数字] 的串，结果与 strtod_c 逐位相同。
/// 下面的十的指数 e 已并入小数位数，有效数字不计前导零；值为 0 时不看指数恒收。收的范围分两层：
/// 有效数字不超过 2^53 且 e 在 ±22 以内时一次乘或除；否则有效数字至多 19 位、e 在 [-64, 64] 内时走 128 位乘法，
/// 舍入落在判不清的半数附近才不收。其余写法一律不收，由调用方回落 strtod_c；编译器按扩展精度算浮点(32 位 x87)时恒不收
/// </summary>
/// <param name="str">数字串，不要求 '\0' 结尾</param>
/// <param name="lens">长度</param>
/// <param name="out">输出：结果，必须非 NULL；返回 ERR_FAILED 时不写</param>
/// <returns>ERR_OK 已算出；ERR_FAILED 不在快路径范围内(不代表串非法)</returns>
int32_t strtod_fast(const char *str, size_t lens, double *out);
/// <summary>
/// (指针, 长度) 的十进制浮点文本转 double，严格判定：整段必须被消费完、不接受空串、上溢即拒
/// ——三条都不是 strtod 自带的，上溢只有 errno 认得出来。
/// 下溢同样置 ERANGE 但返回的是正确的次正规数（DOUBLE 列的常规输出），放行。
/// strtod 直接认出的 "Infinity"/"-Infinity"/"NaN" 字面量是 PostgreSQL float 列的正常输出，
/// 不置 ERANGE 因而放行，由业务自行处置（test_pgsql_reader_double_bounds 锁了这条契约）。
/// mysql / pgsql 两侧的文本协议共用，别再各写一份
/// </summary>
/// <param name="data">源字节段(可非 NUL 结尾)</param>
/// <param name="lens">源字节数；0 视为失败</param>
/// <param name="val">输出：解析结果；返回 ERR_FAILED 时不写</param>
/// <returns>ERR_OK 成功；ERR_FAILED 空串/超 128 字节/有残留字符/上溢</returns>
int32_t strtod_s(const void *data, size_t lens, double *val);

#endif//STRCONV_H_
