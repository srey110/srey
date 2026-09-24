#include "crypt/base64.h"
#include "crypt/crypt_pub.h"

// 解码表的 256 项取值，按字节值排列：非 base64 字符（含 CR/LF/'='）为 0xFF；X 决定每项怎么展开
#define B64DE_LIST(X) \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0x3E), X(0xFF), X(0xFF), X(0xFF), X(0x3F), \
    X(0x34), X(0x35), X(0x36), X(0x37), X(0x38), X(0x39), X(0x3A), X(0x3B), X(0x3C), X(0x3D), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0x00), X(0x01), X(0x02), X(0x03), X(0x04), X(0x05), X(0x06), X(0x07), X(0x08), X(0x09), X(0x0A), X(0x0B), X(0x0C), X(0x0D), X(0x0E), \
    X(0x0F), X(0x10), X(0x11), X(0x12), X(0x13), X(0x14), X(0x15), X(0x16), X(0x17), X(0x18), X(0x19), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0x1A), X(0x1B), X(0x1C), X(0x1D), X(0x1E), X(0x1F), X(0x20), X(0x21), X(0x22), X(0x23), X(0x24), X(0x25), X(0x26), X(0x27), X(0x28), \
    X(0x29), X(0x2A), X(0x2B), X(0x2C), X(0x2D), X(0x2E), X(0x2F), X(0x30), X(0x31), X(0x32), X(0x33), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), \
    X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF), X(0xFF)
#define B64DE_ID(v) v
// 预移位到 4 字节大端写出的位置，非法字符只占最低字节
#define B64DE_SH(v, s) ((v) == 0xFF ? 0xFFu : (uint32_t)(v) << (s))
#define B64DE_S0(v) B64DE_SH(v, 26)
#define B64DE_S1(v) B64DE_SH(v, 20)
#define B64DE_S2(v) B64DE_SH(v, 14)
#define B64DE_S3(v) B64DE_SH(v, 8)
// 双字符编码表的一行：首字符固定为 a，次字符依次取完整字母表
#define B64_ROW(a) \
    { a, 'A' }, { a, 'B' }, { a, 'C' }, { a, 'D' }, { a, 'E' }, { a, 'F' }, { a, 'G' }, { a, 'H' }, \
    { a, 'I' }, { a, 'J' }, { a, 'K' }, { a, 'L' }, { a, 'M' }, { a, 'N' }, { a, 'O' }, { a, 'P' }, \
    { a, 'Q' }, { a, 'R' }, { a, 'S' }, { a, 'T' }, { a, 'U' }, { a, 'V' }, { a, 'W' }, { a, 'X' }, \
    { a, 'Y' }, { a, 'Z' }, { a, 'a' }, { a, 'b' }, { a, 'c' }, { a, 'd' }, { a, 'e' }, { a, 'f' }, \
    { a, 'g' }, { a, 'h' }, { a, 'i' }, { a, 'j' }, { a, 'k' }, { a, 'l' }, { a, 'm' }, { a, 'n' }, \
    { a, 'o' }, { a, 'p' }, { a, 'q' }, { a, 'r' }, { a, 's' }, { a, 't' }, { a, 'u' }, { a, 'v' }, \
    { a, 'w' }, { a, 'x' }, { a, 'y' }, { a, 'z' }, { a, '0' }, { a, '1' }, { a, '2' }, { a, '3' }, \
    { a, '4' }, { a, '5' }, { a, '6' }, { a, '7' }, { a, '8' }, { a, '9' }, { a, '+' }, { a, '/' }

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
// Base64 解码表：按字节值直接索引
static const unsigned char b64de[256] = { B64DE_LIST(B64DE_ID) };
// 解码快路径用的预移位表，下标为字符
static const uint32_t b64d0[256] = { B64DE_LIST(B64DE_S0) };
static const uint32_t b64d1[256] = { B64DE_LIST(B64DE_S1) };
static const uint32_t b64d2[256] = { B64DE_LIST(B64DE_S2) };
static const uint32_t b64d3[256] = { B64DE_LIST(B64DE_S3) };
// 两个字符一组的编码表：下标为 12 位，[0] 对应高 6 位
static const char b64en2[4096][2] = {
    B64_ROW('A'), B64_ROW('B'), B64_ROW('C'), B64_ROW('D'), B64_ROW('E'), B64_ROW('F'), B64_ROW('G'), B64_ROW('H'),
    B64_ROW('I'), B64_ROW('J'), B64_ROW('K'), B64_ROW('L'), B64_ROW('M'), B64_ROW('N'), B64_ROW('O'), B64_ROW('P'),
    B64_ROW('Q'), B64_ROW('R'), B64_ROW('S'), B64_ROW('T'), B64_ROW('U'), B64_ROW('V'), B64_ROW('W'), B64_ROW('X'),
    B64_ROW('Y'), B64_ROW('Z'), B64_ROW('a'), B64_ROW('b'), B64_ROW('c'), B64_ROW('d'), B64_ROW('e'), B64_ROW('f'),
    B64_ROW('g'), B64_ROW('h'), B64_ROW('i'), B64_ROW('j'), B64_ROW('k'), B64_ROW('l'), B64_ROW('m'), B64_ROW('n'),
    B64_ROW('o'), B64_ROW('p'), B64_ROW('q'), B64_ROW('r'), B64_ROW('s'), B64_ROW('t'), B64_ROW('u'), B64_ROW('v'),
    B64_ROW('w'), B64_ROW('x'), B64_ROW('y'), B64_ROW('z'), B64_ROW('0'), B64_ROW('1'), B64_ROW('2'), B64_ROW('3'),
    B64_ROW('4'), B64_ROW('5'), B64_ROW('6'), B64_ROW('7'), B64_ROW('8'), B64_ROW('9'), B64_ROW('+'), B64_ROW('/')
};
size_t bs64_encode(const void *data, const size_t lens, char *out) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t v, w;
    uint16_t e0, e1, e2, e3;
    size_t i = 0, j = 0;
    // 快路径：一次读 8 字节只用前 6 字节，查 4 次双字符表，出 8 个字符一次写
    for (; i + 8 <= lens; i += 6, j += 8) {
        v = _crypt_read64be(p + i);
        memcpy(&e0, b64en2[(v >> 52) & 0xFFF], 2);
        memcpy(&e1, b64en2[(v >> 40) & 0xFFF], 2);
        memcpy(&e2, b64en2[(v >> 28) & 0xFFF], 2);
        memcpy(&e3, b64en2[(v >> 16) & 0xFFF], 2);
        if (IS_LITTLE) {
            w = (uint64_t)e0 | ((uint64_t)e1 << 16) | ((uint64_t)e2 << 32) | ((uint64_t)e3 << 48);
        } else {
            w = ((uint64_t)e0 << 48) | ((uint64_t)e1 << 32) | ((uint64_t)e2 << 16) | (uint64_t)e3;
        }
        memcpy(out + j, &w, sizeof(w));
    }
    // 余下每次消耗 3 字节输入，输出 4 个 Base64 字符
    for (; i + 3 <= lens; i += 3) {
        v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[j] = b64en[v >> 18];
        out[j + 1] = b64en[(v >> 12) & 0x3F];
        out[j + 2] = b64en[(v >> 6) & 0x3F];
        out[j + 3] = b64en[v & 0x3F];
        j += 4;
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
        out[j++] = b64en[(p[i] >> 2) & 0x3F];
        out[j++] = b64en[((p[i] & 0x03) << 4) | ((p[i + 1] >> 4) & 0x0F)];
        out[j++] = b64en[ (p[i + 1] & 0x0F) << 2];
        out[j++] = '=';
        break;
    }
    out[j] = '\0';
    return j;
}
size_t bs64_decode(const char *data, const size_t lens, char *out) {
    const unsigned char *p = (const unsigned char *)data;
    uint32_t a, x, y, v = 0, n = 0;
    uint64_t q;
    size_t i = 0, j = 0, k;
    while (i < lens) {
        // 组对齐时一次读 8 个字符，全是 base64 字符就出 6 字节；否则逐字符处理。
        // 每组用 4 字节写出 3 字节，多出的 1 字节落在下一组的起点，之后必被覆盖
        if (0 == n) {
            for (; i + 8 <= lens; i += 8, j += 6) {
                q = _crypt_read64be(p + i);
                x = b64d0[q >> 56] | b64d1[(q >> 48) & 0xFF] | b64d2[(q >> 40) & 0xFF] | b64d3[(q >> 32) & 0xFF];
                y = b64d0[(q >> 24) & 0xFF] | b64d1[(q >> 16) & 0xFF] | b64d2[(q >> 8) & 0xFF] | b64d3[q & 0xFF];
                if (0 != ((x | y) & 0xFF)) {
                    break;
                }
                _crypt_write32be(out + j, x);
                _crypt_write32be(out + j + 3, y);
            }
            for (; i + 4 <= lens; i += 4, j += 3) {
                x = b64d0[p[i]] | b64d1[p[i + 1]] | b64d2[p[i + 2]] | b64d3[p[i + 3]];
                if (0 != (x & 0xFF)) {
                    break;
                }
                _crypt_write32be(out + j, x);
            }
            if (i == lens) {
                break;
            }
        }
        a = b64de[p[i]];
        if (a < 64) {
            // v 只用低 6*n 位，高位残留由出字节时的移位与 & 0xFF 丢掉
            v = (v << 6) | a;
            if (4 == ++n) {
                out[j++] = (v >> 16) & 0xFF;
                out[j++] = (v >> 8) & 0xFF;
                out[j++] = v & 0xFF;
                n = 0;
            }
            i++;
            continue;
        }
        if ('\r' == p[i]
            || '\n' == p[i]) {
            i++;
            continue;
        }
        if ('=' == p[i]) {
            // RFC 4648：'=' 仅作末尾填充；其后只允许 '='/CR/LF，否则视为伪造截断
            for (k = i + 1; k < lens; k++) {
                if ('=' != p[k]
                    && '\r' != p[k]
                    && '\n' != p[k]) {
                    out[0] = '\0';
                    return 0;
                }
            }
            break;
        }
        out[0] = '\0';
        return 0;
    }
    // 尾组（无填充或 '=' 提前结束）：2 字符→1 字节，3 字符→2 字节；单字符无法构成字节，非法
    switch (n) {
    case 1:
        out[0] = '\0';
        return 0;
    case 2:
        out[j++] = (v >> 4) & 0xFF;
        break;
    case 3:
        out[j++] = (v >> 10) & 0xFF;
        out[j++] = (v >> 2) & 0xFF;
        break;
    }
    out[j] = '\0';
    return j;
}
