#include "crypto/urlraw.h"

static unsigned char hexchars[] = "0123456789ABCDEF";
char *url_encode(const char *str, const size_t len, char *out) {
    register unsigned char c;
    unsigned char const *from, *end;
    from = (unsigned char *)str;
    end = (unsigned char *)str + len;
    unsigned char *to = (unsigned char *)out;
    while (from < end) {
        c = *from++;
        if (c == ' ') {
            *to++ = '+';
        } else if ((c < '0' && c != '-' && c != '.') ||
            (c < 'A' && c > '9') ||
            (c > 'Z' && c < 'a' && c != '_') ||
            (c > 'z')) {
            to[0] = '%';
            to[1] = hexchars[c >> 4];
            to[2] = hexchars[c & 15];
            to += 3;
        } else {
            *to++ = c;
        }
    }
    *to = 0;
    return out;
}
static int32_t _htoi(char *s) {
    int32_t c, value;
    c = ((unsigned char *)s)[0];
    if (isupper(c)) {
        c = tolower(c);
    }
    value = (c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10) * 16;
    c = ((unsigned char *)s)[1];
    if (isupper(c)) {
        c = tolower(c);
    }
    value += c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;
    return (value);
}
size_t url_decode(char *str, size_t len) {
    char *dest = str;
    char *data = str;
    while (len--) {
        if (*data == '+') {
            *dest = ' ';
        } else if (*data == '%'
            && len >= 2
            && isxdigit((int) *(data + 1))
            && isxdigit((int) *(data + 2))) {
            *dest = (char)_htoi(data + 1);
            data += 2;
            len -= 2;
        } else {
            *dest = *data;
        }
        data++;
        dest++;
    }
    *dest = '\0';
    return dest - str;
}
