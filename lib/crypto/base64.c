#include "crypto/base64.h"

/* BASE 64 encode table */
static char base64en[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3',
    '4', '5', '6', '7', '8', '9', '+', '/',
};
#define BASE64_PAD      '='
#define BASE64DE_FIRST  '+'
#define BASE64DE_LAST   'z'
/* ASCII order for BASE 64 decode, -1 in unused character */
static char base64de[] = {
    62,  -1,  -1,  -1,  63,  52,  53,  54,
    55,  56,  57,  58,  59,  60,  61,  -1,
    -1,  -1,  -1,  -1,  -1,  -1,   0,   1,
    2,   3,   4,   5,   6,   7,   8,   9,
    10,  11,  12,  13,  14,  15,  16,  17,
    18,  19,  20,  21,  22,  23,  24,  25,
    -1,  -1,  -1,  -1,  -1,  -1,  26,  27,
    28,  29,  30,  31,  32,  33,  34,  35,
    36,  37,  38,  39,  40,  41,  42,  43,
    44,  45,  46,  47,  48,  49,  50,  51,
};
size_t b64_encode(const char *buf, const size_t len, char *out) {
    int32_t s;
    uint32_t i, j;
    for (i = j = 0; i < len; i++) {
        s = i % 3;
        switch (s) {
        case 0:
            out[j++] = base64en[(buf[i] >> 2) & 0x3F];
            continue;
        case 1:
            out[j++] = base64en[((buf[i - 1] & 0x3) << 4) + ((buf[i] >> 4) & 0xF)];
            continue;
        case 2:
            out[j++] = base64en[((buf[i - 1] & 0xF) << 2) + ((buf[i] >> 6) & 0x3)];
            out[j++] = base64en[buf[i] & 0x3F];
        }
    }
    /* move back */
    i -= 1;
    /* check the last and add padding */
    switch (i % 3) {
    case 0:
        out[j++] = base64en[(buf[i] & 0x3) << 4];
        out[j++] = BASE64_PAD;
        out[j++] = BASE64_PAD;
        break;
    case 1:
        out[j++] = base64en[(buf[i] & 0xF) << 2];
        out[j++] = BASE64_PAD;
        break;
    }
    out[j] = '\0';
    return j;
}
size_t b64_decode(const char *buf, const size_t len, char *out) {
    int32_t c, s;
    uint32_t i, j;
    for (i = j = 0; i < len; i++) {
        s = i % 4;
        if (buf[i] == '=') {
            out[j] = '\0';
            return j;
        }
        if (buf[i] < BASE64DE_FIRST
            || buf[i] > BASE64DE_LAST
            || (c = base64de[buf[i] - BASE64DE_FIRST]) == -1) {
            return 0;
        }
        switch (s) {
        case 0:
            out[j] = ((uint32_t)c << 2) & 0xFF;
            continue;
        case 1:
            out[j++] += ((uint32_t)c >> 4) & 0x3;
            if (i < (len - 3) || buf[len - 2] != '=') {
                out[j] = ((uint32_t)c & 0xF) << 4;
            }
            continue;
        case 2:
            out[j++] += ((uint32_t)c >> 2) & 0xF;
            if (i < (len - 2) || buf[len - 1] != '=') {
                out[j] = ((uint32_t)c & 0x3) << 6;
            }
            continue;
        case 3:
            out[j++] += (uint8_t)c;
        }
    }
    out[j] = '\0';
    return j;
}
