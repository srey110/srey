#include "crypt/xor.h"

// 多轮时两轮并成一趟扫描：同一字节连过两轮再往后走；轮数为奇数时单出的一轮照常走
void *xor_encode(const char key[4], const size_t round, void *data, const size_t lens) {
    if (0 == lens) {
        return data;
    }
    char *p = (char *)data;
    char a, b;
    size_t i, j;
    for (i = 0; i + 1 < round; i += 2) {
        a = ((p[0] + key[1]) ^ key[2]) ^ key[3];
        b = ((a + key[1]) ^ key[2]) ^ key[3];
        p[0] = b;
        for (j = 1; j < lens; j++) {
            a = (a + p[j]) ^ key[0];
            b = (b + a) ^ key[0];
            p[j] = b;
        }
    }
    if (i < round) {
        p[0] = ((p[0] + key[1]) ^ key[2]) ^ key[3];
        for (j = 1; j < lens; j++) {
            p[j] = (p[j - 1] + p[j]) ^ key[0];
        }
    }
    return data;
}
void *xor_decode(const char key[4], const size_t round, void *data, const size_t lens) {
    if (0 == lens) {
        return data;
    }
    char *p = (char *)data;
    for (size_t i = 0; i < round; i++) {
        for (size_t j = lens - 1; j > 0; j--) {
            p[j] = (p[j] ^ key[0]) - p[j - 1];
        }
        p[0] = ((p[0] ^ key[3]) ^ key[2]) - key[1];
    }
    return data;
}
