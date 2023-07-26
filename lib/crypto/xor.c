#include "crypto/xor.h"

char *xor_encode(const char key[4], const size_t round, char *buf, const size_t len) {
    for (size_t i = 0; i < round; i++) {
        buf[0] = ((buf[0] + key[1]) ^ key[2]) ^ key[3];
        for (size_t j = 1; j < len; j++) {
            buf[j] = (buf[j - 1] + buf[j]) ^ key[0];
        }
    }
    return buf;
}
char *xor_decode(const char key[4], const size_t round, char *buf, const size_t len) {
    for (size_t i = 0; i < round; i++) {
        for (size_t j = len - 1; j > 0; j--) {
            buf[j] = (buf[j] ^ key[0]) - buf[j - 1];
        }
        buf[0] = ((buf[0] ^ key[3]) ^ key[2]) - key[1];
    }
    return buf;
}
