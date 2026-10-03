#include "base/bits.h"
#include "base/macro_log.h"

uint32_t pow2_ceil(uint32_t n) {
    if (0 == n || 0 == (n & (n - 1))) {
        return n;
    }
    ASSERTAB(n <= 0x80000000u, "pow2_ceil overflow.");
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n + 1;
}
