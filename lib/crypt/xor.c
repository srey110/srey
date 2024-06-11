#include "crypt/xor.h"

char *xor_encode(const char key[4], const size_t round, char *data, const size_t lens) {
    for (size_t i = 0; i < round; i++) {
        data[0] = ((data[0] + key[1]) ^ key[2]) ^ key[3];
        for (size_t j = 1; j < lens; j++) {
            data[j] = (data[j - 1] + data[j]) ^ key[0];
        }
    }
    return data;
}
char *xor_decode(const char key[4], const size_t round, char *data, const size_t lens) {
    for (size_t i = 0; i < round; i++) {
        for (size_t j = lens - 1; j > 0; j--) {
            data[j] = (data[j] ^ key[0]) - data[j - 1];
        }
        data[0] = ((data[0] ^ key[3]) ^ key[2]) - key[1];
    }
    return data;
}
