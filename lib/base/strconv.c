#include "base/strconv.h"
#include "base/err.h"
#include "base/bits.h"
#include "base/bytes.h"
#include "base/macro_log.h"
#include <locale.h>
#if defined(OS_DARWIN) || defined(OS_BSD)
    #include <xlocale.h>
#endif

// 浮点按扩展精度求值(32 位 x87)时一次乘除也会两次舍入，strtod_fast 的快路径连同下面的 10 的幂表整段关掉
#if (defined(FLT_EVAL_METHOD) && 0 != FLT_EVAL_METHOD && 1 != FLT_EVAL_METHOD) \
    || (!defined(FLT_EVAL_METHOD) && defined(__FLT_EVAL_METHOD__) && 0 != __FLT_EVAL_METHOD__ && 1 != __FLT_EVAL_METHOD__)
    #define STRTOD_FAST_OFF
#endif
#define STRTOD_POW10_MIN (-64)
#define STRTOD_POW10_MAX 64
// 生成 _hex_val 表用：逐项算出一个字节的十六进制值，非法为 -1
#define _HEX1(c)  (int8_t)(((c) >= '0' && (c) <= '9') ? (c) - '0' : ((c) >= 'a' && (c) <= 'f') ? (c) - 'a' + 10 \
                            : ((c) >= 'A' && (c) <= 'F') ? (c) - 'A' + 10 : -1)
#define _HEX4(n)  _HEX1(n), _HEX1((n) + 1), _HEX1((n) + 2), _HEX1((n) + 3)
#define _HEX16(n) _HEX4(n), _HEX4((n) + 4), _HEX4((n) + 8), _HEX4((n) + 12)
// 十六进制字符的值，非法字符为 -1（= ERR_FAILED）
static const int8_t _hex_val[256] = {
    _HEX16(0), _HEX16(16), _HEX16(32), _HEX16(48),
    _HEX16(64), _HEX16(80), _HEX16(96), _HEX16(112),
    _HEX16(128), _HEX16(144), _HEX16(160), _HEX16(176),
    _HEX16(192), _HEX16(208), _HEX16(224), _HEX16(240)
};
static const char hex_char_upper[16] = {
    '0', '1', '2', '3',
    '4', '5', '6', '7',
    '8', '9', 'A', 'B',
    'C', 'D', 'E', 'F'
};
static const char hex_char_lower[16] = {
    '0', '1', '2', '3',
    '4', '5', '6', '7',
    '8', '9', 'a', 'b',
    'c', 'd', 'e', 'f'
};
// "00" ~ "99" 依次排开，u64tostr 十进制每次查出两位
static const char _dec_pairs[] =
    "00010203040506070809101112131415161718192021222324252627282930313233343536373839"
    "40414243444546474849505152535455565758596061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";
#ifndef STRTOD_FAST_OFF
// 10^e(e 在 STRTOD_POW10_MIN~MAX)规格化后的高 128 位，每个 e 两个值(高 64 位、低 64 位)，不向上取整
static const uint64_t _strtod_pow10[] = {
    0xA87FEA27A539E9A5ULL, 0x3F2398D747B36224ULL, 0xD29FE4B18E88640EULL, 0x8EEC7F0D19A03AADULL,
    0x83A3EEEEF9153E89ULL, 0x1953CF68300424ACULL, 0xA48CEAAAB75A8E2BULL, 0x5FA8C3423C052DD7ULL,
    0xCDB02555653131B6ULL, 0x3792F412CB06794DULL, 0x808E17555F3EBF11ULL, 0xE2BBD88BBEE40BD0ULL,
    0xA0B19D2AB70E6ED6ULL, 0x5B6ACEAEAE9D0EC4ULL, 0xC8DE047564D20A8BULL, 0xF245825A5A445275ULL,
    0xFB158592BE068D2EULL, 0xEED6E2F0F0D56712ULL, 0x9CED737BB6C4183DULL, 0x55464DD69685606BULL,
    0xC428D05AA4751E4CULL, 0xAA97E14C3C26B886ULL, 0xF53304714D9265DFULL, 0xD53DD99F4B3066A8ULL,
    0x993FE2C6D07B7FABULL, 0xE546A8038EFE4029ULL, 0xBF8FDB78849A5F96ULL, 0xDE98520472BDD033ULL,
    0xEF73D256A5C0F77CULL, 0x963E66858F6D4440ULL, 0x95A8637627989AADULL, 0xDDE7001379A44AA8ULL,
    0xBB127C53B17EC159ULL, 0x5560C018580D5D52ULL, 0xE9D71B689DDE71AFULL, 0xAAB8F01E6E10B4A6ULL,
    0x9226712162AB070DULL, 0xCAB3961304CA70E8ULL, 0xB6B00D69BB55C8D1ULL, 0x3D607B97C5FD0D22ULL,
    0xE45C10C42A2B3B05ULL, 0x8CB89A7DB77C506AULL, 0x8EB98A7A9A5B04E3ULL, 0x77F3608E92ADB242ULL,
    0xB267ED1940F1C61CULL, 0x55F038B237591ED3ULL, 0xDF01E85F912E37A3ULL, 0x6B6C46DEC52F6688ULL,
    0x8B61313BBABCE2C6ULL, 0x2323AC4B3B3DA015ULL, 0xAE397D8AA96C1B77ULL, 0xABEC975E0A0D081AULL,
    0xD9C7DCED53C72255ULL, 0x96E7BD358C904A21ULL, 0x881CEA14545C7575ULL, 0x7E50D64177DA2E54ULL,
    0xAA242499697392D2ULL, 0xDDE50BD1D5D0B9E9ULL, 0xD4AD2DBFC3D07787ULL, 0x955E4EC64B44E864ULL,
    0x84EC3C97DA624AB4ULL, 0xBD5AF13BEF0B113EULL, 0xA6274BBDD0FADD61ULL, 0xECB1AD8AEACDD58EULL,
    0xCFB11EAD453994BAULL, 0x67DE18EDA5814AF2ULL, 0x81CEB32C4B43FCF4ULL, 0x80EACF948770CED7ULL,
    0xA2425FF75E14FC31ULL, 0xA1258379A94D028DULL, 0xCAD2F7F5359A3B3EULL, 0x096EE45813A04330ULL,
    0xFD87B5F28300CA0DULL, 0x8BCA9D6E188853FCULL, 0x9E74D1B791E07E48ULL, 0x775EA264CF55347DULL,
    0xC612062576589DDAULL, 0x95364AFE032A819DULL, 0xF79687AED3EEC551ULL, 0x3A83DDBD83F52204ULL,
    0x9ABE14CD44753B52ULL, 0xC4926A9672793542ULL, 0xC16D9A0095928A27ULL, 0x75B7053C0F178293ULL,
    0xF1C90080BAF72CB1ULL, 0x5324C68B12DD6338ULL, 0x971DA05074DA7BEEULL, 0xD3F6FC16EBCA5E03ULL,
    0xBCE5086492111AEAULL, 0x88F4BB1CA6BCF584ULL, 0xEC1E4A7DB69561A5ULL, 0x2B31E9E3D06C32E5ULL,
    0x9392EE8E921D5D07ULL, 0x3AFF322E62439FCFULL, 0xB877AA3236A4B449ULL, 0x09BEFEB9FAD487C2ULL,
    0xE69594BEC44DE15BULL, 0x4C2EBE687989A9B3ULL, 0x901D7CF73AB0ACD9ULL, 0x0F9D37014BF60A10ULL,
    0xB424DC35095CD80FULL, 0x538484C19EF38C94ULL, 0xE12E13424BB40E13ULL, 0x2865A5F206B06FB9ULL,
    0x8CBCCC096F5088CBULL, 0xF93F87B7442E45D3ULL, 0xAFEBFF0BCB24AAFEULL, 0xF78F69A51539D748ULL,
    0xDBE6FECEBDEDD5BEULL, 0xB573440E5A884D1BULL, 0x89705F4136B4A597ULL, 0x31680A88F8953030ULL,
    0xABCC77118461CEFCULL, 0xFDC20D2B36BA7C3DULL, 0xD6BF94D5E57A42BCULL, 0x3D32907604691B4CULL,
    0x8637BD05AF6C69B5ULL, 0xA63F9A49C2C1B10FULL, 0xA7C5AC471B478423ULL, 0x0FCF80DC33721D53ULL,
    0xD1B71758E219652BULL, 0xD3C36113404EA4A8ULL, 0x83126E978D4FDF3BULL, 0x645A1CAC083126E9ULL,
    0xA3D70A3D70A3D70AULL, 0x3D70A3D70A3D70A3ULL, 0xCCCCCCCCCCCCCCCCULL, 0xCCCCCCCCCCCCCCCCULL,
    0x8000000000000000ULL, 0x0000000000000000ULL, 0xA000000000000000ULL, 0x0000000000000000ULL,
    0xC800000000000000ULL, 0x0000000000000000ULL, 0xFA00000000000000ULL, 0x0000000000000000ULL,
    0x9C40000000000000ULL, 0x0000000000000000ULL, 0xC350000000000000ULL, 0x0000000000000000ULL,
    0xF424000000000000ULL, 0x0000000000000000ULL, 0x9896800000000000ULL, 0x0000000000000000ULL,
    0xBEBC200000000000ULL, 0x0000000000000000ULL, 0xEE6B280000000000ULL, 0x0000000000000000ULL,
    0x9502F90000000000ULL, 0x0000000000000000ULL, 0xBA43B74000000000ULL, 0x0000000000000000ULL,
    0xE8D4A51000000000ULL, 0x0000000000000000ULL, 0x9184E72A00000000ULL, 0x0000000000000000ULL,
    0xB5E620F480000000ULL, 0x0000000000000000ULL, 0xE35FA931A0000000ULL, 0x0000000000000000ULL,
    0x8E1BC9BF04000000ULL, 0x0000000000000000ULL, 0xB1A2BC2EC5000000ULL, 0x0000000000000000ULL,
    0xDE0B6B3A76400000ULL, 0x0000000000000000ULL, 0x8AC7230489E80000ULL, 0x0000000000000000ULL,
    0xAD78EBC5AC620000ULL, 0x0000000000000000ULL, 0xD8D726B7177A8000ULL, 0x0000000000000000ULL,
    0x878678326EAC9000ULL, 0x0000000000000000ULL, 0xA968163F0A57B400ULL, 0x0000000000000000ULL,
    0xD3C21BCECCEDA100ULL, 0x0000000000000000ULL, 0x84595161401484A0ULL, 0x0000000000000000ULL,
    0xA56FA5B99019A5C8ULL, 0x0000000000000000ULL, 0xCECB8F27F4200F3AULL, 0x0000000000000000ULL,
    0x813F3978F8940984ULL, 0x4000000000000000ULL, 0xA18F07D736B90BE5ULL, 0x5000000000000000ULL,
    0xC9F2C9CD04674EDEULL, 0xA400000000000000ULL, 0xFC6F7C4045812296ULL, 0x4D00000000000000ULL,
    0x9DC5ADA82B70B59DULL, 0xF020000000000000ULL, 0xC5371912364CE305ULL, 0x6C28000000000000ULL,
    0xF684DF56C3E01BC6ULL, 0xC732000000000000ULL, 0x9A130B963A6C115CULL, 0x3C7F400000000000ULL,
    0xC097CE7BC90715B3ULL, 0x4B9F100000000000ULL, 0xF0BDC21ABB48DB20ULL, 0x1E86D40000000000ULL,
    0x96769950B50D88F4ULL, 0x1314448000000000ULL, 0xBC143FA4E250EB31ULL, 0x17D955A000000000ULL,
    0xEB194F8E1AE525FDULL, 0x5DCFAB0800000000ULL, 0x92EFD1B8D0CF37BEULL, 0x5AA1CAE500000000ULL,
    0xB7ABC627050305ADULL, 0xF14A3D9E40000000ULL, 0xE596B7B0C643C719ULL, 0x6D9CCD05D0000000ULL,
    0x8F7E32CE7BEA5C6FULL, 0xE4820023A2000000ULL, 0xB35DBF821AE4F38BULL, 0xDDA2802C8A800000ULL,
    0xE0352F62A19E306EULL, 0xD50B2037AD200000ULL, 0x8C213D9DA502DE45ULL, 0x4526F422CC340000ULL,
    0xAF298D050E4395D6ULL, 0x9670B12B7F410000ULL, 0xDAF3F04651D47B4CULL, 0x3C0CDD765F114000ULL,
    0x88D8762BF324CD0FULL, 0xA5880A69FB6AC800ULL, 0xAB0E93B6EFEE0053ULL, 0x8EEA0D047A457A00ULL,
    0xD5D238A4ABE98068ULL, 0x72A4904598D6D880ULL, 0x85A36366EB71F041ULL, 0x47A6DA2B7F864750ULL,
    0xA70C3C40A64E6C51ULL, 0x999090B65F67D924ULL, 0xD0CF4B50CFE20765ULL, 0xFFF4B4E3F741CF6DULL,
    0x82818F1281ED449FULL, 0xBFF8F10E7A8921A4ULL, 0xA321F2D7226895C7ULL, 0xAFF72D52192B6A0DULL,
    0xCBEA6F8CEB02BB39ULL, 0x9BF4F8A69F764490ULL, 0xFEE50B7025C36A08ULL, 0x02F236D04753D5B4ULL,
    0x9F4F2726179A2245ULL, 0x01D762422C946590ULL, 0xC722F0EF9D80AAD6ULL, 0x424D3AD2B7B97EF5ULL,
    0xF8EBAD2B84E0D58BULL, 0xD2E0898765A7DEB2ULL, 0x9B934C3B330C8577ULL, 0x63CC55F49F88EB2FULL,
    0xC2781F49FFCFA6D5ULL, 0x3CBF6B71C76B25FBULL
};
#endif
#if defined(OS_WIN)
static _locale_t g_numeric_c;
#else
static locale_t g_numeric_c;
#endif

// 19 位以内累加不会溢出 uint64，逐位只判是不是数字，上界留到最后比一次；更长的才走逐位先判后乘
int32_t strtou64(const char *str, size_t lens, uint64_t max, uint64_t *out) {
    uint64_t cut, lim;
    uint64_t v = 0;
    uint64_t d;
    size_t i;
    if (0 == lens
        || NULL == str) {
        return ERR_FAILED;
    }
    if (lens <= 19) {
        for (i = 0; i < lens; i++) {
            d = (uint64_t)(uint8_t)str[i] - '0';
            if (d > 9) {
                return ERR_FAILED;
            }
            v = v * 10 + d;
        }
        if (v > max) {
            return ERR_FAILED;
        }
        *out = v;
        return ERR_OK;
    }
    cut = max / 10;
    lim = max % 10;
    for (i = 0; i < lens; i++) {
        if (str[i] < '0'
            || str[i] > '9') {
            return ERR_FAILED;
        }
        d = (uint64_t)(str[i] - '0');
        // 先判后乘, 免得溢出之后再回头查; 顺带把 max 上界一并管了, 不必事后再比
        if (v > cut
            || (v == cut && d > lim)) {
            return ERR_FAILED;
        }
        v = v * 10 + d;
    }
    *out = v;
    return ERR_OK;
}
int32_t strtoi64(const void *data, size_t lens, int64_t *val) {
    const char *s = (const char *)data;
    int32_t neg = (lens > 0 && NULL != s && '-' == s[0]);
    uint64_t mag;
    if (0 != neg) {
        s++;
        lens--;
    }
    if (ERR_OK != strtou64(s, lens, 0 != neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX, &mag)) {
        return ERR_FAILED;
    }
    // INT64_MIN 的绝对值超出 int64_t，取负前先单独挑出来，免得 -(int64_t)mag 落进未定义行为
    *val = (0 == neg) ? (int64_t)mag
                      : ((uint64_t)INT64_MAX + 1 == mag ? INT64_MIN : -(int64_t)mag);
    return ERR_OK;
}
// 把 v 按 base 写进 out，先数出位数(0 算 1 位)，再从末位往前写，返回位数
static inline size_t _u64_digits(char *out, uint64_t v, uint32_t base) {
    uint64_t t = v;
    size_t n = 1;
    while (t >= base) {
        t /= base;
        n++;
    }
    size_t i = n;
    do {
        out[--i] = hex_char_lower[v % base];
        v /= base;
    } while (0 != i);
    return n;
}
// 十进制专用的 _u64_digits：按 100 一组数位，从末位往前每次从 _dec_pairs 查两位写，最高位剩一位时单写
static inline size_t _u64_digits10(char *out, uint64_t v) {
    uint64_t t = v;
    uint64_t q;
    size_t n = 1;
    size_t i;
    while (t >= 100) {
        t /= 100;
        n += 2;
    }
    n += (t >= 10) ? 1 : 0;
    i = n;
    while (v >= 100) {
        q = v / 100;
        i -= 2;
        memcpy(out + i, _dec_pairs + (v - q * 100) * 2, 2);
        v = q;
    }
    // 循环出来的 v 即数位时剩下的 t，占最高的 1 或 2 位
    if (v >= 10) {
        memcpy(out, _dec_pairs + v * 2, 2);
    } else {
        out[0] = (char)('0' + v);
    }
    return n;
}
size_t u64tostr(char *out, uint64_t v, uint32_t base) {
    size_t n;
    ASSERTAB(base >= 2 && base <= 16, ERRSTR_INVPARAM);
    // 10 进制两位一查表；16 用常量调一次，内联后除法折成移位；其余进制按变量除
    if (10 == base) {
        n = _u64_digits10(out, v);
    } else if (16 == base) {
        n = _u64_digits(out, v, 16);
    } else {
        n = _u64_digits(out, v, base);
    }
    out[n] = '\0';
    return n;
}
size_t i64tostr(char *out, int64_t v, uint32_t base) {
    if (v < 0) {
        *out = '-';
        return 1 + u64tostr(out + 1, 0 - (uint64_t)v, base);// 先转无符号再取负，INT64_MIN 也不溢出
    }
    return u64tostr(out, (uint64_t)v, base);
}
char *tohex(const void *buf, size_t len, char *out, int32_t lower) {
    const char *tbl = lower ? hex_char_lower : hex_char_upper;
    size_t j = 0;
    unsigned char *p = (unsigned char *)buf;
    for (size_t i = 0; i < len; ++i) {
        out[j] = tbl[(p[i] / 16)];
        ++j;
        out[j] = tbl[(p[i] % 16)];
        ++j;
    }
    out[j] = '\0';
    return out;
}
int32_t fromhex(char c) {
    return _hex_val[(unsigned char)c];
}
void locale_init(void) {
#ifdef OS_WIN
    g_numeric_c = _create_locale(LC_NUMERIC, "C");
    ASSERTAB(NULL != g_numeric_c, "_create_locale(LC_NUMERIC, \"C\") failed.");
#else
    g_numeric_c = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    ASSERTAB((locale_t)0 != g_numeric_c, ERRORSTR(ERRNO));
#endif
}
void locale_free(void) {
#ifdef OS_WIN
    if (NULL != g_numeric_c) {
        _free_locale(g_numeric_c);
        g_numeric_c = NULL;
    }
#else
    if ((locale_t)0 != g_numeric_c) {
        freelocale(g_numeric_c);
        g_numeric_c = (locale_t)0;
    }
#endif
}
double strtod_c(const char *str, char **endptr) {
#ifdef OS_WIN
    return _strtod_l(str, endptr, g_numeric_c);
#else
    return strtod_l(str, endptr, g_numeric_c);
#endif
}
#ifndef STRTOD_FAST_OFF
// sig(非 0、至多 19 位的精确值) × 10^exp10 按 128 位乘出 IEEE 位型；舍入落在判不清的半数附近时返 ERR_FAILED 交回 strtod。
// exp10 须在 STRTOD_POW10_MIN~MAX，这个量程内结果一定是规格化数
static int32_t _strtod_mul_pow10(uint64_t sig, int32_t exp10, uint64_t *raw) {
    const uint64_t *p10 = &_strtod_pow10[(exp10 - STRTOD_POW10_MIN) * 2];
    int32_t exp2 = (exp10 * 217706 - 4128768) >> 16;
    int32_t lz = (int32_t)clz64(sig);
    uint64_t sig1 = sig << lz;
    uint64_t hi, lo, hi2, add;
    exp2 -= lz;
    lo = mul64_128(sig1, p10[0], &hi);
    if ((hi & 0x1FF) - 1 >= 0x1FE) {
        mul64_128(sig1, p10[1], &hi2);
        add = lo + hi2;
        if (add + 1 <= 1) {
            return ERR_FAILED;
        }
        hi += (add < lo || add < hi2);
    }
    lz = (hi < ((uint64_t)1 << 63));
    hi <<= lz;
    exp2 = exp2 - lz + 64;
    hi += (0 != (hi & ((uint64_t)1 << 10))) ? ((uint64_t)1 << 10) : 0;
    if (hi < ((uint64_t)1 << 10)) {
        hi = (uint64_t)1 << 63;
        exp2++;
    }
    *raw = ((uint64_t)(exp2 + 63 + 1023) << 52) | ((hi >> 11) & 0x000FFFFFFFFFFFFFULL);
    return ERR_OK;
}
#endif
int32_t strtod_fast(const char *str, size_t lens, double *out) {
#ifdef STRTOD_FAST_OFF
    (void)str;
    (void)lens;
    (void)out;
    return ERR_FAILED;
#else
    static const double pow10[] = {
        1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
    };
    const char *p = str;
    const char *end = str + lens;
    const char *mark;
    const char *lead;
    uint64_t sig = 0;
    uint64_t raw;
    uint32_t c;
    size_t ndig;
    int32_t exp10 = 0;
    int32_t eval = 0;
    int32_t eneg = 0;
    int32_t neg = 0;
    double d;
    if (p < end && '-' == *p) {
        neg = 1;
        p++;
    }
    mark = p;
    while (p < end && '0' == *p) {
        p++;
    }
    lead = p;
    while (p < end && (c = (uint32_t)((uint8_t)*p - '0')) <= 9) {
        sig = sig * 10 + c;
        p++;
    }
    if (p == mark) {
        return ERR_FAILED;
    }
    ndig = (size_t)(p - lead);
    if (p < end && '.' == *p) {
        mark = ++p;
        if (0 == ndig) {
            while (p < end && '0' == *p) {
                p++;
            }
        }
        lead = p;
        while (p < end && (c = (uint32_t)((uint8_t)*p - '0')) <= 9) {
            sig = sig * 10 + c;
            p++;
        }
        if (p == mark) {
            return ERR_FAILED;
        }
        ndig += (size_t)(p - lead);
        if ((size_t)(p - mark) > 1000) {
            return ERR_FAILED;
        }
        exp10 = -(int32_t)(p - mark);
    }
    if (ndig > 19) {
        return ERR_FAILED;
    }
    if (p < end && ('e' == *p || 'E' == *p)) {
        p++;
        if (p < end && ('+' == *p || '-' == *p)) {
            eneg = ('-' == *p);
            p++;
        }
        mark = p;
        while (p < end && (c = (uint32_t)((uint8_t)*p - '0')) <= 9) {
            if (eval < 100000) {
                eval = eval * 10 + (int32_t)c;
            }
            p++;
        }
        if (p == mark) {
            return ERR_FAILED;
        }
    }
    if (p != end) {
        return ERR_FAILED;
    }
    exp10 += eneg ? -eval : eval;
    if (0 == sig) {
        *out = neg ? -0.0 : 0.0;
        return ERR_OK;
    }
    if (sig <= ((uint64_t)1 << 53)
        && exp10 >= -22
        && exp10 <= 22) {
        d = (double)sig;
        d = (exp10 < 0) ? d / pow10[-exp10] : d * pow10[exp10];
        *out = neg ? -d : d;
        return ERR_OK;
    }
    if (exp10 < STRTOD_POW10_MIN
        || exp10 > STRTOD_POW10_MAX
        || ERR_OK != _strtod_mul_pow10(sig, exp10, &raw)) {
        return ERR_FAILED;
    }
    raw |= (uint64_t)neg << 63;
    memcpy(out, &raw, sizeof(raw));
    return ERR_OK;
#endif
}
// 先在原串上走 strtod_fast 的精确快路径，它不收的写法("+1"、".5"、"Infinity"、十六进制、上溢等)
// 才拷进 tmp 补 NUL 退回 strtod_c，判定与原先逐字相同
int32_t strtod_s(const void *data, size_t lens, double *val) {
    char tmp[128];
    // 长度先挡：快路径不经过 tmp，不先挡的话 ≥128 字节的串会被它收下；也保证下面拷进 tmp 带 NUL 装得下
    if (0 == lens
        || lens >= sizeof(tmp)) {
        return ERR_FAILED;
    }
    if (ERR_OK == strtod_fast((const char *)data, lens, val)) {
        return ERR_OK;
    }
    memcpy(tmp, data, lens);
    tmp[lens] = '\0';
    char *end;
    errno = 0;
    double d = strtod_c(tmp, &end);
    if ((size_t)(end - tmp) != lens
        || (ERANGE == errno && (HUGE_VAL == d || -HUGE_VAL == d))) {
        return ERR_FAILED;
    }
    *val = d;
    return ERR_OK;
}
