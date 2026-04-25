#include "crypt/base64.h"

// Base64 编码字符表
static const char b64en[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3',
    '4', '5', '6', '7', '8', '9', '+', '/',
};
// Base64 解码字符表（按 ASCII 顺序排列，未使用位填 -1）
static const char b64de[] = {
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
size_t bs64_encode(const void *data, const size_t lens, char *out) {
    const unsigned char *p = (const unsigned char *)data;
    size_t i = 0, j = 0;
    // 主循环：每次消耗 3 字节输入，输出 4 个 Base64 字符
    for (; i + 3 <= lens; i += 3) {
        out[j++] = b64en[(p[i]     >> 2) & 0x3F];
        out[j++] = b64en[((p[i]     & 0x03) << 4) | ((p[i + 1] >> 4) & 0x0F)];
        out[j++] = b64en[((p[i + 1] & 0x0F) << 2) | ((p[i + 2] >> 6) & 0x03)];
        out[j++] = b64en[  p[i + 2] & 0x3F];
    }
    // 尾部处理：剩余 0、1 或 2 字节，补充 '=' 填充
    switch (lens - i) {
    case 1:
        out[j++] = b64en[(p[i] >> 2) & 0x3F];
        out[j++] = b64en[(p[i] & 0x03) << 4];
        out[j++] = '=';
        out[j++] = '=';
        break;
    case 2:
        out[j++] = b64en[(p[i]     >> 2) & 0x3F];
        out[j++] = b64en[((p[i]     & 0x03) << 4) | ((p[i + 1] >> 4) & 0x0F)];
        out[j++] = b64en[ (p[i + 1] & 0x0F) << 2];
        out[j++] = '=';
        break;
    }
    out[j] = '\0';
    return j;
}
size_t bs64_decode(const char *data, const size_t lens, char *out) {
    int32_t c;
    uint32_t i, j, idx = 0;
    for (i = j = 0; i < lens; i++) {
        if ('\r' == data[i]
            || '\n' == data[i]) {
            continue;
        }
        if ('=' == data[i]) {
            out[j] = '\0';
            return j;
        }
        if (data[i] < '+'
            || data[i] > 'z'
            || (c = b64de[data[i] - '+']) == -1) {
            return 0;
        }
        switch (idx % 4) {
        case 0:
            out[j] = ((uint32_t)c << 2) & 0xFF;
            break;
        case 1:
            out[j++] += ((uint32_t)c >> 4) & 0x3;
            if (idx < (lens - 3) || data[lens - 2] != '=') {
                out[j] = ((uint32_t)c & 0xF) << 4;
            }
            break;
        case 2:
            out[j++] += ((uint32_t)c >> 2) & 0xF;
            if (idx < (lens - 2) || data[lens - 1] != '=') {
                out[j] = ((uint32_t)c & 0x3) << 6;
            }
            break;
        case 3:
            out[j++] += (uint8_t)c;
            break;
        }
        idx++;
    }
    out[j] = '\0';
    return j;
}
