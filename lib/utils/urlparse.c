#include "utils/urlparse.h"
#include "utils/utils.h"
#include "crypt/urlraw.h"

// 解析协议类型（scheme）：只认首个 '/' '?' '#' 之前的 "://"，未找到则原样返回。
// 范围必须限定，否则相对 URL 会被当成绝对 URL 切开
static char *_url_scheme(buf_ctx *scheme, char *cur, size_t lens) {
    for (size_t i = 0; i < lens; i++) {
        if ('/' == cur[i]
            || '?' == cur[i]
            || '#' == cur[i]) {
            return cur;
        }
        if (':' != cur[i]) {
            continue;
        }
        // RFC 3986 §3.1：scheme 到首个 ':' 为止且不得为空，其后不是 "//" 即不是绝对 URL
        if (0 == i
            || i + 2 >= lens
            || '/' != cur[i + 1]
            || '/' != cur[i + 2]) {
            return cur;
        }
        scheme->data = cur;
        scheme->lens = i;
        return cur + i + 3;
    }
    return cur;
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
// 反向找 what（无 memrchr 的平台也要能编）
static char *_url_rchr(char *cur, char what, size_t lens) {
    for (size_t i = lens; i > 0; i--) {
        if (what == cur[i - 1]) {
            return cur + i - 1;
        }
    }
    return NULL;
}
// 以 what 字符为界解析两段字段（buf1:buf2），返回指向 what 之后的指针。
// last 非 0 时以最后一个 what 为界：userinfo 按 RFC 3986 §3.2 就该取最后一个 '@'，
// 取第一个会把 "user@host@evil.com" 的 host 认成 "host@evil.com"
static inline char *_url_parse_two(buf_ctx *buf1, buf_ctx *buf2, char *cur, char what, size_t lens, int32_t last) {
    char *pos = (0 != last) ? _url_rchr(cur, what, lens) : memchr(cur, what, lens);
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
// 解析路径部分（直到 '?' 或 '#'），返回指向下一段（查询或片段）的指针；
// *phash 交回 [cur, cur + lens) 里的首个 '#'(没有为 NULL)，它也就是锚点的分界
static inline char *_url_path(buf_ctx *path, char *cur, size_t lens, char **phash) {
    char *hash = memchr(cur, '#', lens);
    *phash = hash;
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
// 单个 param 重组所需字节数：paramlens 累加与 url_reorg_param 写入共用本式，分开写必漂移
static inline size_t _url_param_need(const url_param *p, size_t offset) {
    return (offset > 0 ? 1 : 0) + p->key.lens
        + (NULL != p->val.data ? 1 : 0) + p->val.lens;
}
// 解析查询字符串：先按 '&' 切段，段内首个 '=' 之前为 key、之后为 val；
// 段内无 '=' 为无值参数(val 空)，最多解析 URL_MAX_PARAM 个，个数写回 *nparam。
// 空段与空名段(如 "=v")都不入表，故"已记录的 param 必有 key.lens > 0"是本函数保证的不变式，
// 四个消费方(url_parse 累加 / url_reorg_param / url_get_param / lpub 建表)无须各自跳过空名。
// param[] 不预清零，每条的 key 与 val 都在这里写满
static void _url_param(url_param *param, char *cur, size_t lens, int32_t *nparam) {
    char *end = cur + lens;
    char *amp;
    char *eq;
    char *segend;
    url_param *tmp;
    size_t seglens;
    int32_t i = 0;
    while (cur < end
        && i < URL_MAX_PARAM) {
        amp = memchr(cur, '&', (size_t)(end - cur));
        segend = (NULL == amp) ? end : amp;
        seglens = (size_t)(segend - cur);
        eq = (seglens > 0) ? memchr(cur, '=', seglens) : NULL;
        if (seglens > 0
            && cur != eq) {
            tmp = &param[i++];
            tmp->key.data = cur;
            if (NULL == eq) {
                tmp->key.lens = seglens;
                tmp->val.data = NULL;
                tmp->val.lens = 0;
            } else {
                tmp->key.lens = (size_t)(eq - cur);
                tmp->val.data = eq + 1;
                tmp->val.lens = (size_t)(segend - eq - 1);
            }
        }
        if (NULL == amp) {
            break;
        }
        cur = amp + 1;
    }
    *nparam = i;
}
int32_t url_parse(url_ctx *ctx, const char *url, size_t lens, int8_t sep, int32_t decode) {
    // 只清头部标量与各 buf_ctx 字段（128 字节）。param / segs / buf 三个大数组共约 4KB，
    // 有效范围由 nparam / npath / lens 划定，越界部分谁都不读——整体清是每个 HTTP 请求白付一次
    ZERO(ctx, offsetof(url_ctx, param));
    ctx->sep = sep;
    ctx->decode = decode;
    char *urlbuf;
    char *hash;
    int32_t dseg = decode;
    int32_t dparam;
    if (ctx->decode) {
        if (lens >= sizeof(ctx->buf)) {
            LOG_WARN("url too long.");
            return ERR_FAILED;
        }
        // 上面的 lens >= sizeof(buf) 已挡过，strict 那档失败不会发生
        (void)copy_bounded(url, lens, ctx->buf, sizeof(ctx->buf), 1);
        urlbuf = ctx->buf;
        if (NULL == memchr(url, '%', lens)) {
            dseg = 0;
        }
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
    if (0 == auth_len) {
        ctx->host.data = cur;
    } else {
        cur = _url_parse_two(&ctx->user, &ctx->psw, cur, '@', auth_len, 1);
        remain = lens - (size_t)(cur - urlbuf);
        if (0 == remain) {
            return ERR_OK;
        }
        // host:port 段限定在 authority 内（长度 = auth_end - cur），不可用 remain：
        // 否则 "http://host?k=v" 会把 "host?k=v" 当 host:port，丢失 query 参数。
        // authority 段内不含 '/'，_url_parse_two 必走 '/' fallback _url_split + return cur+lens=auth_end
        cur = _url_parse_two(&ctx->host, &ctx->port, cur, '/', (size_t)(auth_end - cur), 0);
        remain = lens - (size_t)(cur - urlbuf);
        if (0 == remain) {
            return ERR_OK;
        }
    }
    //路径
    buf_ctx path = { 0 };
    cur = _url_path(&path, cur, remain, &hash);
    if (!buf_empty(&path)) {
        // 移除前导
        char *pp = path.data;
        size_t plen = path.lens;
        if (sep == pp[0]) {
            pp++;
            plen--;
        }
        buf_ctx *psegs = ctx->segs;
        ctx->npath = split(pp, plen, (const char *)&ctx->sep, 1, &psegs, URL_MAX_PATH_DEPTH, 0);
        if (ERR_FAILED == ctx->npath) {
            return ERR_FAILED;
        }
        //逐段解码并累计重组后总长(与 url_reorg_path 输出对齐)
        for (int32_t i = 0; i < ctx->npath; i++) {
            if (dseg && ctx->segs[i].lens > 0) {
                ctx->segs[i].lens = url_decode(ctx->segs[i].data, ctx->segs[i].lens, 0);
            }
            ctx->pathlens += (ctx->segs[i].lens + 1);
        }
    }
    remain = lens - (size_t)(cur - urlbuf);
    if (0 == remain) {
        return ERR_OK;
    }
    if (NULL != hash) {
        size_t alens = remain - (size_t)(hash - cur) - 1;
        if (alens > 0) {
            ctx->anchor.data = hash + 1;
            ctx->anchor.lens = alens;
        }
        remain = (size_t)(hash - cur);
    }
    if (0 == remain) {
        return ERR_OK;
    }
    _url_param(ctx->param, cur, remain, &ctx->nparam);
    dparam = dseg || (0 != decode && NULL != memchr(cur, '+', remain));
    url_param *p;
    for (int32_t i = 0; i < ctx->nparam; i++) {
        p = &ctx->param[i];
        if (dparam) {
            p->key.lens = url_decode(p->key.data, p->key.lens, 1);
        }
        if (!buf_empty(&p->val)) {
            if (dparam && p->val.lens > 0) {
                p->val.lens = url_decode(p->val.data, p->val.lens, 1);
            }
        }
        ctx->paramlens += _url_param_need(p, ctx->paramlens);
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
    size_t need, offset = 0;
    url_param *p;
    for (int32_t i = 0; i < ctx->nparam; i++) {
        p = &ctx->param[i];
        // 放不下则截断；无值参数(val.data 为 NULL)不写 '='
        need = _url_param_need(p, offset);
        if (offset + need + 1 > cap) {
            break;
        }
        if (offset > 0) {
            param[offset++] = '&';
        }
        memcpy(param + offset, p->key.data, p->key.lens);
        offset += p->key.lens;
        if (NULL != p->val.data) {
            param[offset++] = '=';
            if (p->val.lens > 0) {
                memcpy(param + offset, p->val.data, p->val.lens);
                offset += p->val.lens;
            }
        }
    }
    param[offset] = '\0';
    return offset;
}
buf_ctx *url_get_param(url_ctx *ctx, const char *key) {
    url_param *param = NULL;
    size_t klens = strlen(key);
    // 倒着扫: 同名参数取最后一个, 后写的盖掉先写的
    for (int32_t i = ctx->nparam - 1; i >= 0; i--) {
        param = &ctx->param[i];
        if (buf_compare(&param->key, key, klens)) {
            return &param->val;
        }
    }
    return NULL;
}
