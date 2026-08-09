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
