#include "protocol/urlparse.h"
#include "utils/utils.h"

static char *_scheme(buf_ctx *scheme, char *cur, size_t lens) {
    char *pos = memstr(1, cur, lens, "://", 3);
    if (NULL == pos) {
        return cur;
    }
    scheme->data = cur;
    scheme->lens = pos - cur;
    return pos + 3;
}
static void _split(buf_ctx *buf1, buf_ctx *buf2, char *cur, size_t lens) {
    char *pos = memchr(cur, ':', lens);
    if (NULL == pos) {
        buf1->data = cur;
        buf1->lens = lens;
        return;
    }
    buf1->data = cur;
    buf1->lens = pos - cur;
    size_t size = lens - (pos + 1 - cur);
    if (size > 0) {
        buf2->data = pos + 1;
        buf2->lens = size;
    }
}
static char *_parse_two(buf_ctx *buf1, buf_ctx *buf2, char *cur, char what, size_t lens) {
    char *pos = memchr(cur, what, lens);
    if (NULL == pos) {
        if ('/' == what) {
            _split(buf1, buf2, cur, lens);
            return cur + lens;
        } else {
            return cur;
        }
    }
    if (pos == cur){
        return pos + 1;
    }
    _split(buf1, buf2, cur, pos - cur);
    return pos + 1;
}
static char *_path(buf_ctx *path, char *cur, size_t lens) {
    char *pos = memchr(cur, '?', lens);
    if (NULL == pos) {
        pos = memchr(cur, '#', lens);
        if (NULL == pos) {
            path->data = cur;
            path->lens = lens;
            return cur + lens;
        } else {
            if (pos != cur) {
                path->data = cur;
                path->lens = pos - cur;
            }
            return pos;
        }
    }
    if (pos == cur) {
        return pos + 1;
    }
    path->data = cur;
    path->lens = pos - cur;
    return pos + 1;
}
static size_t _anchor(buf_ctx *anchor, char *cur, size_t lens) {
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
static void _param(url_param *param, char *cur, size_t lens) {
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
        if (NULL == pos){
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
//[协议类型]://[访问资源需要的凭证信息]@[服务器地址]:[端口号]/[资源层级UNIX文件路径][文件名]?[查询]#[片段ID]
void url_parse(url_ctx *ctx, char *url, size_t lens) {
    ZERO(ctx, sizeof(url_ctx));
    size_t remain = lens;
    char *cur = _scheme(&ctx->scheme, url, remain);
    remain = lens - (cur - url);
    if (0 == remain){
        return;
    }
    cur = _parse_two(&ctx->user, &ctx->psw, cur, '@', remain);
    remain = lens - (cur - url);
    if (0 == remain) {
        return;
    }
    cur = _parse_two(&ctx->host, &ctx->port, cur, '/', remain);
    remain = lens - (cur - url);
    if (0 == remain) {
        return;
    }
    cur = _path(&ctx->path, cur, remain);
    remain = lens - (cur - url);
    if (0 == remain) {
        return;
    }
    remain = _anchor(&ctx->anchor, cur, remain);
    if (0 == remain) {
        return;
    }
    _param(ctx->param, cur, remain);
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
