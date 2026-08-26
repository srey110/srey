#include "protocol/prots_pub.h"
#include "utils/utils.h"

uint32_t parse_usec_frac(const char *str) {
    uint32_t usec = 0;
    const char *dot = strchr(str, '.');
    if (NULL != dot) {
        dot++;
        int32_t mult = 100000;
        for (int32_t i = 0; i < 6 && dot[i] >= '0' && dot[i] <= '9'; i++, mult /= 10) {
            usec += (uint32_t)((dot[i] - '0') * mult);
        }
    }
    return usec;
}
int32_t parse_colon_triple(const char *str, const uint32_t max[3], uint32_t val[3]) {
    uint32_t got[3] = { 0, 0, 0 };
    const char *p = str;
    const char *digit;
    uint64_t v;
    int32_t n = 0;
    while (n < 3) {
        digit = p;
        while ('0' <= *p
               && '9' >= *p) {
            p++;
        }
        if (p == digit) {
            break;
        }
        if (ERR_OK != str2u64(digit, (size_t)(p - digit), max[n], &v)) {
            // 超上界即整串作废,不留半截结果:调用方拿三段值算时刻,半截比全无更难查
            return 0;
        }
        got[n] = (uint32_t)v;
        n++;
        if (':' != *p) {
            break;
        }
        p++;
    }
    if (0 == n) {
        return 0;
    }
    val[0] = got[0];
    val[1] = got[1];
    val[2] = got[2];
    return n;
}
int32_t parse_int64_strict(const void *data, size_t lens, int64_t *val) {
    const char *s = (const char *)data;
    int32_t neg = (lens > 0 && NULL != s && '-' == s[0]);
    uint64_t mag;
    if (0 != neg) {
        s++;
        lens--;
    }
    if (ERR_OK != str2u64(s, lens, 0 != neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX, &mag)) {
        return ERR_FAILED;
    }
    // INT64_MIN 的绝对值超出 int64_t，取负前先单独挑出来，免得 -(int64_t)mag 落进未定义行为
    *val = (0 == neg) ? (int64_t)mag
                      : ((uint64_t)INT64_MAX + 1 == mag ? INT64_MIN : -(int64_t)mag);
    return ERR_OK;
}
int32_t parse_double_strict(const void *data, size_t lens, double *val) {
    char tmp[128];
    if (0 == lens
        || ERR_OK != copy_bounded(data, lens, tmp, sizeof(tmp), 1)) {
        return ERR_FAILED;
    }
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
