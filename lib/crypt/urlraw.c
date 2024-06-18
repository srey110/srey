#include "crypt/urlraw.h"

static const unsigned char hexchars[] = "0123456789ABCDEF";
char *url_encode(const char *data, const size_t lens, char *out) {
    register unsigned char c;
    unsigned char const *from, *end;
    from = (unsigned char *)data;
    end = (unsigned char *)data + lens;
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
size_t url_decode(char *data, size_t lens) {
    char *dest = data;
    char *p = data;
    while (lens--) {
        if (*p == '+') {
            *dest = ' ';
        } else if (*p == '%'
            && lens >= 2
            && isxdigit((int) *(p + 1))
            && isxdigit((int) *(p + 2))) {
            *dest = (char)_htoi(p + 1);
            p += 2;
            lens -= 2;
        } else {
            *dest = *p;
        }
        p++;
        dest++;
    }
    *dest = '\0';
    return (size_t)(dest - data);
}
