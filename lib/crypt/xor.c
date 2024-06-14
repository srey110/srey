#include "crypt/xor.h"

void *xor_encode(const char key[4], const size_t round, void *data, const size_t lens) {
    char *p = (char *)data;
    for (size_t i = 0; i < round; i++) {
        p[0] = ((p[0] + key[1]) ^ key[2]) ^ key[3];
        for (size_t j = 1; j < lens; j++) {
            p[j] = (p[j - 1] + p[j]) ^ key[0];
        }
    }
    return data;
}
void *xor_decode(const char key[4], const size_t round, void *data, const size_t lens) {
    char *p = (char *)data;
    for (size_t i = 0; i < round; i++) {
        for (size_t j = lens - 1; j > 0; j--) {
            p[j] = (p[j] ^ key[0]) - p[j - 1];
        }
        p[0] = ((p[0] ^ key[3]) ^ key[2]) - key[1];
    }
    return data;
}
