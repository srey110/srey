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
int32_t _url_path_split(char *path, size_t lens, int8_t sep,
                        buf_ctx *segs, int32_t cap) {
    int32_t n = 0;
    size_t remain = lens;
    char *p = path;
    char *q;
    size_t slen;
    // 标准切分:保留空段(连续/尾随 sep 产生 len==0 段),段数 = sep 数 + 1
    for (;;) {
        if (n >= cap) {
            LOG_WARN("path depth too long.");
            return ERR_FAILED;
        }
        q = memchr(p, sep, remain);
        slen = (NULL != q) ? (size_t)(q - p) : remain;
        segs[n].data = p;
        segs[n].lens = slen;
        n++;
        if (NULL == q) {
            break;
        }
        remain -= (slen + 1);
        p = q + 1;
    }
    return n;
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
// 解析查询字符串中的 key=value 参数对，以 '&' 分隔，最多解析 URL_MAX_PARAM 个
static void _url_param(url_param *param, char *cur, size_t lens) {
    char *pos;
    char *start = cur;
    url_param *tmp;
    size_t rmain, size;
    for (int32_t i = 0; i < URL_MAX_PARAM; i++) {
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
int32_t url_parse(url_ctx *ctx, const char *url, size_t lens, int8_t sep, int32_t decode) {
    ZERO(ctx, sizeof(url_ctx));
    ctx->sep = sep;
    ctx->decode = decode;
    char *urlbuf;
    if (ctx->decode) {
        if (lens >= sizeof(ctx->buf)) {
            LOG_WARN("url too long.");
            return ERR_FAILED;
        }
        memcpy(ctx->buf, url, lens);
        urlbuf = ctx->buf;
    } else {
        urlbuf = (char *)url;
    }
    //协议类型（scheme） "://" 
    char *cur = _url_scheme(&ctx->scheme, urlbuf, lens);
    size_t remain = lens - (size_t)(cur - urlbuf);
    if (0 == remain) {
        return ERR_OK;
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
    remain = lens - (size_t)(cur - urlbuf);
    if (0 == remain) {
        return ERR_OK;
    }
    // host:port 段限定在 authority 内（长度 = auth_end - cur），不可用 remain：
    // 否则 "http://host?k=v" 会把 "host?k=v" 当 host:port，丢失 query 参数。
    // authority 段内不含 '/'，_url_parse_two 必走 '/' fallback _url_split + return cur+lens=auth_end
    cur = _url_parse_two(&ctx->host, &ctx->port, cur, '/', (size_t)(auth_end - cur));
    remain = lens - (size_t)(cur - urlbuf);
    if (0 == remain) {
        return ERR_OK;
    }
    //路径
    buf_ctx path = { 0 };
    cur = _url_path(&path, cur, remain);
    if (!buf_empty(&path)) {
        // 移除前导
        char *pp = path.data;
        size_t plen = path.lens;
        if (sep == pp[0]) {
            pp++;
            plen--;
        }
        ctx->npath = _url_path_split(pp, plen, ctx->sep, ctx->segs, URL_MAX_PATH_DEPTH);
        if (ERR_FAILED == ctx->npath) {
            return ERR_FAILED;
        }
        //逐段解码并累计重组后总长(与 url_reorg_path 输出对齐)
        for (int32_t i = 0; i < ctx->npath; i++) {
            if (ctx->decode && ctx->segs[i].lens > 0) {
                ctx->segs[i].lens = url_decode(ctx->segs[i].data, ctx->segs[i].lens, 0);
            }
            ctx->pathlens += (ctx->segs[i].lens + 1);
        }
    }
    remain = lens - (size_t)(cur - urlbuf);
    if (0 == remain) {
        return ERR_OK;
    }
    remain = _url_anchor(&ctx->anchor, cur, remain);
    if (0 == remain) {
        return ERR_OK;
    }
    _url_param(ctx->param, cur, remain);
    url_param *p;
    for (int32_t i = 0; i < URL_MAX_PARAM; i++) {
        p = &ctx->param[i];
        if (buf_empty(&p->key)) {
            break;
        }
        if (ctx->decode && p->key.lens > 0) {
            p->key.lens = url_decode(p->key.data, p->key.lens, 1);
        }
        if (!buf_empty(&p->val)) {
            if (ctx->decode && p->val.lens > 0) {
                p->val.lens = url_decode(p->val.data, p->val.lens, 1);
            }
        }
    }
    return ERR_OK;
}
size_t url_reorg_path(url_ctx *ctx, char *path, size_t cap) {
    if (0 == cap) {
        return 0;
    }
    size_t offset = 0;
    for (int32_t i = 0; i < ctx->npath; i++) {
        // 段前分隔符 + 段内容 + 结尾 '\0' 放不下则截断
        if (offset + 1 + ctx->segs[i].lens + 1 > cap) {
            break;
        }
        path[offset++] = ctx->sep;
        memcpy(path + offset, ctx->segs[i].data, ctx->segs[i].lens);
        offset += ctx->segs[i].lens;
    }
    path[offset] = '\0';
    return offset;
}
size_t url_reorg_param(url_ctx *ctx, char *param, size_t cap) {
    if (0 == cap) {
        return 0;
    }
    size_t offset = 0;
    url_param *p;
    for (int32_t i = 0; i < URL_MAX_PARAM; i++) {
        p = &ctx->param[i];
        if (buf_empty(&p->key)) {
            break;
        }
        // '&' + key + '=' + val + '\0' 放不下则截断
        size_t need = (i > 0 ? 1 : 0) + p->key.lens + 1 + p->val.lens;
        if (offset + need + 1 > cap) {
            break;
        }
        if (i > 0) {
            param[offset++] = '&';
        }
        memcpy(param + offset, p->key.data, p->key.lens);
        offset += p->key.lens;
        param[offset++] = '=';
        if (p->val.lens > 0) {
            memcpy(param + offset, p->val.data, p->val.lens);
            offset += p->val.lens;
        }
    }
    param[offset] = '\0';
    return offset;
}
buf_ctx *url_get_param(url_ctx *ctx, const char *key) {
    url_param *param = NULL;
    size_t klens = strlen(key);
    for (int32_t i = 0; i < URL_MAX_PARAM; i++) {
        param = &ctx->param[i];
        if (NULL == param->key.data
            || 0 == param->key.lens) {
            break;
        }
        if (buf_compare(&param->key, key, klens)) {
            return &param->val;
        }
    }
    return NULL;
}
