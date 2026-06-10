#include "protocol/urlparse.h"
#include "utils/utils.h"
#include "crypt/urlraw.h"

// 解析协议类型（scheme），找到 "://" 分隔符，返回其后的指针；未找到则原样返回
static char *_url_scheme(buf_ctx *scheme, char *cur, size_t lens) {
    char *pos = memstr(1, cur, lens, "://", 3);
    if (NULL == pos) {
        return cur;
    }
    scheme->data = cur;
    scheme->lens = pos - cur;
    return pos + 3;
}
// 以冒号为分隔符将当前段拆分为两部分（如 host:port 或 user:password）
// IPv6 地址须以方括号包裹（RFC 3986 §3.2.2），如 [::1]:8080；方括号内的冒号不作分隔符
static void _url_split(buf_ctx *buf1, buf_ctx *buf2, char *cur, size_t lens) {
    if (lens > 0 && '[' == *cur) {
        char *bracket = memchr(cur, ']', lens);
        if (NULL == bracket) {
            buf1->data = cur;
            buf1->lens = lens;
            return;
        }
        size_t after = lens - (size_t)(bracket + 1 - cur);
        if (after >= 1 && ':' == bracket[1]) {
            buf1->data = cur;
            buf1->lens = (size_t)(bracket + 1 - cur);
            size_t plen = after - 1;
            if (plen > 0) {
                buf2->data = bracket + 2;
                buf2->lens = plen;
            }
        } else {
            buf1->data = cur;
            buf1->lens = lens;
        }
        return;
    }
    char *pos = memchr(cur, ':', lens);
    if (NULL == pos) {
        buf1->data = cur;
        buf1->lens = lens;
        return;
    }
    buf1->data = cur;
    buf1->lens = pos - cur;
    size_t size = lens - (size_t)(pos + 1 - cur);
    if (size > 0) {
        buf2->data = pos + 1;
        buf2->lens = size;
    }
}
// 以 what 字符为界解析两段字段（buf1:buf2），返回指向 what 之后的指针
static char *_url_parse_two(buf_ctx *buf1, buf_ctx *buf2, char *cur, char what, size_t lens) {
    char *pos = memchr(cur, what, lens);
    if (NULL == pos) {
        if ('/' == what) {
            // 未找到 '/'，整段作为 host:port
            _url_split(buf1, buf2, cur, lens);
            return cur + lens;
        } else {
            return cur;
        }
    }
    if (pos == cur) {
        return pos + 1;
    }
    _url_split(buf1, buf2, cur, pos - cur);
    return pos + 1;
}
// 解析路径部分（直到 '?' 或 '#'），返回指向下一段（查询或片段）的指针
static char *_url_path(buf_ctx *path, char *cur, size_t lens) {
    char *hash = memchr(cur, '#', lens);
    size_t search_lens = (NULL != hash) ? (size_t)(hash - cur) : lens;
    char *pos = memchr(cur, '?', search_lens);
    if (NULL == pos) {
        if (NULL == hash) {
            path->data = cur;
            path->lens = lens;
            return cur + lens;
        }
        if (hash != cur) {
            path->data = cur;
            path->lens = (size_t)(hash - cur);
        }
        return hash;
    }
    if (pos == cur) {
        return pos + 1;
    }
    path->data = cur;
    path->lens = (size_t)(pos - cur);
    return pos + 1;
}
// 从当前段中提取锚点（# 之后的部分），返回锚点之前的查询参数段长度
static size_t _url_anchor(buf_ctx *anchor, char *cur, size_t lens) {
    char *pos = memchr(cur, '#', lens);
    if (NULL == pos) {
        return lens;
    }
    size_t size;
    if (pos == cur) {
        size = lens - 1;
        if (size > 0) {
            anchor->data = pos + 1;
            anchor->lens = size;
        }
        return 0;
    }
    size = lens - (pos + 1 - cur);
    if (size > 0) {
        anchor->data = pos + 1;
        anchor->lens = size;
    }
    return pos - cur;
}
// 解析查询字符串中的 key=value 参数对，以 '&' 分隔，最多解析 MAX_NPARAM 个
static void _url_param(url_param *param, char *cur, size_t lens) {
    char *pos;
    char *start = cur;
    url_param *tmp;
    size_t rmain, size;
    for (int32_t i = 0; i < MAX_NPARAM; i++) {
        tmp = &param[i];
        pos = memchr(cur, '=', lens - (cur - start));
        if (NULL == pos) {
            break;
        }
        tmp->key.data = cur;
        tmp->key.lens = pos - cur;
        cur = pos + 1;
        rmain = lens - (cur - start);
        if (0 == rmain) {
            break;
        }
        pos = memchr(cur, '&', rmain);
        if (NULL == pos) {
            // 最后一个参数
            tmp->val.data = cur;
            tmp->val.lens = rmain;
            break;
        }
        size = pos - cur;
        if (size > 0) {
            tmp->val.data = cur;
            tmp->val.lens = size;
        }
        cur = pos + 1;
    }
}
void url_parse(url_ctx *ctx, const char *url, size_t lens) {
    ZERO(ctx, sizeof(url_ctx));
    size_t copylen = lens < sizeof(ctx->buf) - 1 ? lens : sizeof(ctx->buf) - 1;
    memcpy(ctx->buf, url, copylen);
    // ctx->buf[copylen] 已由 ZERO 清零，无需再写 '\0'
    char *cur = _url_scheme(&ctx->scheme, ctx->buf, copylen);
    size_t remain = copylen - (size_t)(cur - ctx->buf);
    if (0 == remain) {
        return;
    }
    // RFC 3986 §3.2：authority 段以 '/' '?' '#' 或 url 末尾结束；
    // '@' 仅在 authority 内分隔 userinfo，不可匹配 path/query/fragment 中的 '@'
    size_t auth_len = remain;
    for (size_t i = 0; i < remain; i++) {
        if ('/' == cur[i] || '?' == cur[i] || '#' == cur[i]) {
            auth_len = i;
            break;
        }
    }
    char *auth_end = cur + auth_len;
    cur = _url_parse_two(&ctx->user, &ctx->psw, cur, '@', auth_len);
    remain = copylen - (size_t)(cur - ctx->buf);
    if (0 == remain) {
        return;
    }
    // host:port 段限定在 authority 内（长度 = auth_end - cur），不可用 remain：
    // 否则 "http://host?k=v" 会把 "host?k=v" 当 host:port，丢失 query 参数。
    // authority 段内不含 '/'，_url_parse_two 必走 '/' fallback _url_split + return cur+lens=auth_end
    cur = _url_parse_two(&ctx->host, &ctx->port, cur, '/', (size_t)(auth_end - cur));
    remain = copylen - (size_t)(cur - ctx->buf);
    if (0 == remain) {
        return;
    }
    cur = _url_path(&ctx->path, cur, remain);
    if (!buf_empty(&ctx->path)) {
        ctx->path.lens = url_decode(ctx->path.data, ctx->path.lens);
    }
    remain = copylen - (size_t)(cur - ctx->buf);
    if (0 == remain) {
        return;
    }
    remain = _url_anchor(&ctx->anchor, cur, remain);
    if (0 == remain) {
        return;
    }
    _url_param(ctx->param, cur, remain);
    url_param *p;
    for (int32_t i = 0; i < MAX_NPARAM; i++) {
        p = &ctx->param[i];
        if (buf_empty(&p->key)) {
            break;
        }
        p->key.lens = url_decode(p->key.data, p->key.lens);
        if (!buf_empty(&p->val)) {
            p->val.lens = url_decode(p->val.data, p->val.lens);
        }
    }
}
buf_ctx *url_get_param(url_ctx *ctx, const char *key) {
    url_param *param = NULL;
    size_t klens = strlen(key);
    for (int32_t i = 0; i < MAX_NPARAM; i++) {
        param = &ctx->param[i];
        if (NULL == param->key.data
            || 0 == param->key.lens) {
            break;
        }
        if (buf_icompare(&param->key, key, klens)) {
            return &param->val;
        }
    }
    return NULL;
}
