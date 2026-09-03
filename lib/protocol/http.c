#include "protocol/http.h"
#include "event/event.h"
#include "crypt/urlraw.h"
#include "containers/sarray.h"
#include "utils/utils.h"

// 头部缓冲区从 cur 起的剩余字节数。形参名避开 head：宏体里 (p)->head 的 head 也会被替换
#define HEAD_REMAIN(p, cur) ((p)->head.lens - (size_t)((cur) - (char *)(p)->head.data))

typedef enum parse_status{
    INIT = 0,   // 初始状态，等待头部
    CONTENT,    // 已解析头部，等待 Content-Length 指定的数据体
    CHUNKED,    // 分块传输模式
    TILLCLOSE,  // 响应无 CL/TE，body 由连接关闭界定（RFC 7230 §3.3.3 规则 7）
    INIT_NOBODY // 同 INIT，但紧随的那一条响应按"无报文体"处理（HEAD 用，见 http_set_method）
}parse_status;
typedef struct http_pack_ctx {
    int32_t chunked;          // 0=非 chunked，1=chunked 起始包，2=chunked 数据包
    int32_t chunked_last;     // 最近一次见到的 Transfer-Encoding 行，其末尾 token 是否为 chunked
    buf_ctx head;             // 原始头部数据（含第一行和所有字段）
    buf_ctx data;             // 数据体
    buf_ctx status[3];        // 第一行拆分：status[0]=方法/版本，status[1]=状态码/路径，status[2]=描述/版本
    array_ctx header;         // 所有头部字段列表（元素 http_header_ctx）
}http_pack_ctx;

// 裁剪 [*start, *end) 区间首尾的 OWS (SP/HTAB)；全为 OWS 时收成 *start 处的空区间。
// trim 只读不写，故这里丢弃 const 安全（底层缓冲本就是可写的解析区）
static void _http_trim_ows(const char **start, const char **end) {
    size_t lens = 0;
    const char *cur = trim((char *)*start, (size_t)(*end - *start), &lens);
    if (NULL == cur) {
        *end = *start;
        return;
    }
    *start = cur;
    *end = cur + lens;
}
int32_t _http_check_keyval(http_header_ctx *head,
                           const char *key, size_t klen,
                           const char *val, size_t vlen) {
    if (!buf_icompare(&head->key, key, klen)) {
        return ERR_FAILED;
    }
    if (NULL == val) {
        return ERR_OK;
    }
    const char *p = (const char *)head->value.data;
    const char *end = (const char *)head->value.data + head->value.lens;
    const char *tstart;
    const char *tend;
    while (p < end) {
        tstart = p;
        // 找下个 ',' 或末尾
        while (p < end && ',' != *p) {
            p++;
        }
        tend = p;
        _http_trim_ows(&tstart, &tend);
        // 大小写不敏感全等比较
        if ((size_t)(tend - tstart) == vlen
            && 0 == STRNCMP(tstart, val, vlen)) {
            return ERR_OK;
        }
        if (p < end) {
            p++;// 跳过 ','
        }
    }
    return ERR_FAILED;
}
// 解析 Content-Length 字段的十进制值；拒绝空值与非纯数字（含正负号与空白）。
// 两侧 OWS 已由 _http_parse_field 的 trim 剥掉，此处 value 即纯字段值
static int32_t _http_parse_content_length(http_header_ctx *field, size_t *out) {
    char *vbuf = (char *)field->value.data;
    size_t vlen = field->value.lens;
    if (0 == vlen) {
        return ERR_FAILED;
    }
    uint64_t val;
    if (ERR_OK != str2u64(vbuf, vlen, (uint64_t)SIZE_MAX, &val)) {
        return ERR_FAILED;
    }
    *out = (size_t)val;
    return ERR_OK;
}
// 检查头部值经逗号分隔后的最后一个 token 是否等于指定字符串（大小写不敏感）
static int32_t _http_check_lastval(http_header_ctx *head, const char *val, size_t vlen) {
    const char *data = (const char *)head->value.data;
    const char *end = data + head->value.lens;
    const char *start;
    const char *tstart;
    const char *tend;
    for (;;) {
        start = end;
        while (start > data && ',' != *(start - 1)) {
            start--;
        }
        tstart = start;
        tend = end;
        _http_trim_ows(&tstart, &tend);
        if (tstart < tend) {
            return (size_t)(tend - tstart) == vlen && 0 == STRNCMP(tstart, val, vlen) ? ERR_OK : ERR_FAILED;
        }
        // RFC 7230 §7：list 末尾/连续逗号引入的空元素须被忽略，跳过该逗号继续找上一个 token
        if (start == data) {
            return ERR_FAILED;
        }
        end = start - 1;
    }
}
// 检查头部字段是否为 Content-Length 或 Transfer-Encoding，更新传输方式。
static int32_t _http_check_transfer(http_pack_ctx *pack, http_header_ctx *field, int32_t *transfer) {
    int32_t is_te = (ERR_OK == _http_check_keyval(field,
                                                  "transfer-encoding", sizeof("transfer-encoding") - 1,
                                                  NULL, 0));
    int32_t is_cl = (ERR_OK == _http_check_keyval(field,
                                                  "content-length", sizeof("content-length") - 1,
                                                  NULL, 0));
    if (is_te) {
        // RFC 7230 §3.3.3：CL 之后再来 TE → 视为 smuggling，拒绝
        if (CONTENT == *transfer) {
            LOG_WARN("HTTP smuggling: Transfer-Encoding after Content-Length.");
            return ERR_FAILED;
        }
        pack->chunked_last = (ERR_OK == _http_check_lastval(field, "chunked", sizeof("chunked") - 1));
        if (CHUNKED != *transfer) {
            *transfer = CHUNKED;
            pack->data.lens = 0;
            pack->chunked = 1;
        }
        return ERR_OK;
    }
    if (is_cl) {
        // RFC 7230 §3.3.3：TE 之后再来 CL → 视为 smuggling，拒绝
        if (CHUNKED == *transfer) {
            LOG_WARN("HTTP smuggling: Content-Length after Transfer-Encoding.");
            return ERR_FAILED;
        }
        size_t new_lens;
        if (ERR_OK != _http_parse_content_length(field, &new_lens)) {
            return ERR_FAILED;
        }
        // RFC 7230 §3.3.2：重复 CL 必须同值，不同值视为 smuggling，拒绝
        if (CONTENT == *transfer) {
            if (new_lens != pack->data.lens) {
                LOG_WARN("HTTP smuggling: duplicate Content-Length with different values (%zu vs %zu).",
                         pack->data.lens, new_lens);
                return ERR_FAILED;
            }
        } else {
            *transfer = CONTENT;
            pack->data.lens = new_lens;
        }
    }
    return ERR_OK;
}
// 是否为 RFC 7230 §2.6 的 HTTP-version（HTTP/DIGIT.DIGIT，恰 8 字节）
static int32_t _http_is_version(buf_ctx *seg) {
    const char *ver = (const char *)seg->data;
    return 8 == seg->lens
        && 0 == memcmp(ver, "HTTP/", 5)
        && ver[5] >= '0' && ver[5] <= '9'
        && '.' == ver[6]
        && ver[7] >= '0' && ver[7] <= '9';
}
int32_t http_code_nobody(int32_t code) {
    return code < 200 || 204 == code || 304 == code;
}
// 收到的这个包是否 RFC 7230 §3.3.3 规则 1 里"一律无报文体"的响应。
// client 为 0 时收到的是请求，一律返 0：本端解析不出方向——_http_parse_status 只要求首段或末段
// 是 HTTP-version，"HTTP/1.1 204 z" 这种伪请求行照样能过，在服务端按响应处理就是一次请求走私
// 是否 1xx 中间响应:首行校验与 _http_nobody_resp 同一套,只是判的区间不同
static int32_t _http_interim_resp(http_pack_ctx *pack, int32_t client) {
    if (0 == client
        || !_http_is_version(&pack->status[0])
        || 3 != pack->status[1].lens) {
        return 0;
    }
    uint64_t code;
    if (ERR_OK != str2u64((const char *)pack->status[1].data, pack->status[1].lens, 999, &code)) {
        return 0;
    }
    return code < 200;
}
static int32_t _http_nobody_resp(http_pack_ctx *pack, int32_t client) {
    if (0 == client
        || !_http_is_version(&pack->status[0])
        || 3 != pack->status[1].lens) {
        return 0;
    }
    uint64_t code;
    if (ERR_OK != str2u64((const char *)pack->status[1].data, pack->status[1].lens, 999, &code)) {
        return 0;
    }
    return http_code_nobody((int32_t)code);
}
// 首行与字段行共用的单趟行扫描：扫到行尾 CRLF 为止，顺带记下行内出现的分隔符位置。
// 只有 CRLF 才算收行，裸 CR、裸 LF 和 NUL 一律拒（RFC 9110 §5.5）：放行裸 LF 会与上游切出
// 不同的头部边界，四道 TE/CL 走私守卫按字段名精确匹配，看不见折进值里的那条 TE。
// mark 为要记录的分隔符（首行传 ' '，字段行传 ':'），按出现顺序最多记 nmark 个写入 marks，
// 实到个数写回 *nout（行内超过 nmark 个时只记前 nmark 个，多出来的归调用方自行处理）。
// 返回行尾 CRLF 的起始位置；未收到完整行或撞上非法字节返回 NULL
static char *_http_scan_line(const char *head, size_t remain, char mark,
                             char **marks, int32_t nmark, int32_t *nout) {
    const char *cur = head;
    size_t scanned = 0;
    *nout = 0;
    while (scanned < remain) {
        if (mark == *cur
            && *nout < nmark) {
            marks[(*nout)++] = (char *)cur;
        }
        if ('\r' == *cur) {
            if (scanned + 1 >= remain
                || '\n' != *(cur + 1)) {
                return NULL;
            }
            return (char *)cur;
        }
        if ('\n' == *cur
            || '\0' == *cur) {
            return NULL;
        }
        cur++;
        scanned++;
    }
    return NULL;
}
// 解析 HTTP 第一行（请求行或状态行），填充 pack->status[0..2]，返回指向第一个头部字段的指针。
// 状态行 HTTP-version 在首段、请求行在末段，故两段须恰有一段是 HTTP-version：
// 请求行末段因此不能含多余 SP，也不能是任意垃圾串
static char *_http_parse_status(http_pack_ctx *pack) {
    char *head = pack->head.data;
    if (0 == pack->head.lens
        || is_ows(*head)) {
        return NULL;
    }
    char *sp[2];
    int32_t nsp;
    char *pcrlf = _http_scan_line(head, HEAD_REMAIN(pack, head), ' ', sp, 2, &nsp);
    if (NULL == pcrlf
        || 2 != nsp) {
        return NULL;
    }
    pack->status[0].data = head;
    pack->status[0].lens = (size_t)(sp[0] - head);
    pack->status[1].data = sp[0] + 1;
    pack->status[1].lens = (size_t)(sp[1] - sp[0] - 1);
    pack->status[2].data = sp[1] + 1;
    pack->status[2].lens = (size_t)(pcrlf - sp[1] - 1);
    if (0 == pack->status[0].lens
        || 0 == pack->status[1].lens) {
        return NULL;
    }
    if (!_http_is_version(&pack->status[0])
        && !_http_is_version(&pack->status[2])) {
        return NULL;
    }
    return pcrlf + CRLF_SIZE;
}
// 解析单个头部字段行（key ":" OWS value CRLF），拒绝空 key 与 obs-fold 续行，成功后 *phead 推进到下一行行首
static int32_t _http_parse_field(http_pack_ctx *pack, char **phead, http_header_ctx *field) {
    char *head = *phead;
    // RFC 7230 §3.2.4：字段行不得以 OWS(SP/HTAB) 开头(obs-fold 续行折叠)，须拒绝防上游折进请求行的边界分歧走私
    if (is_ows(*head)) {
        return ERR_FAILED;
    }
    char *pcolon;
    int32_t ncolon;
    char *pcrlf = _http_scan_line(head, HEAD_REMAIN(pack, head), ':', &pcolon, 1, &ncolon);
    if (NULL == pcrlf
        || 1 != ncolon) {
        return ERR_FAILED;
    }
    field->key.data = head;
    field->key.lens = (size_t)(pcolon - head);
    // RFC 7230 §3.2.6：字段名只能由 token 字符组成（长度为 0 时 is_token 也返回假，一并挡掉）。
    // 这一条同时覆盖了"字段名与冒号之间不许有空白"：SP/HTAB 都不是 token 字符，放行的话
    // `Transfer-Encoding :chunked` 就带着尾随空格绕过 TE/CL 的精确比长匹配，构成请求走私
    if (!is_token((const char *)field->key.data, field->key.lens)) {
        return ERR_FAILED;
    }
    head = pcolon + 1;
    size_t vlens = 0;
    char *vdata = trim(head, (size_t)(pcrlf - head), &vlens);
    field->value.data = (NULL != vdata) ? vdata : head;
    field->value.lens = vlens;
    head = pcrlf + CRLF_SIZE;
    *phead = head;
    return ERR_OK;
}
// 解析全部头部字段，检测 Content-Length/Transfer-Encoding，将字段存入 pack->header
static int32_t _http_parse_head(http_pack_ctx *pack, int32_t *transfer) {
    char *head = _http_parse_status(pack);
    if (NULL == head) {
        return ERR_FAILED;
    }
    http_header_ctx field;
    // head 缓冲由 _http_headlens 保证以首个 \r\n\r\n 结尾，空行即头部结束
    while (0 != memcmp(head, FLAG_CRLF, CRLF_SIZE)) {
        if (ERR_OK != _http_parse_field(pack, &head, &field)) {
            return ERR_FAILED;
        }
        if (ERR_OK != _http_check_transfer(pack, &field, transfer)) {
            return ERR_FAILED;
        }
        array_push_back(&pack->header, &field);
    }
    // 全部头部行都见过之后才能确定 chunked 是否为最后一个 transfer-coding
    if (CHUNKED == *transfer && !pack->chunked_last) {
        LOG_WARN("HTTP smuggling: chunked is not the final transfer-coding.");
        return ERR_FAILED;
    }
    return ERR_OK;
}
// 等待并读取 Content-Length 模式下的数据体，数据完整后重置 ud 状态
static http_pack_ctx *_http_content(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    http_pack_ctx *pack = ud->context;
    if (buffer_size(buf) >= pack->data.lens) {
        if (pack->data.lens > 0) {
            MALLOC(pack->data.data, pack->data.lens);
            ASSERTAB(pack->data.lens == buffer_remove(buf, pack->data.data, pack->data.lens), "copy buffer failed.");
        }
        ud->status = INIT;
        ud->context = NULL;
        return pack;
    } else {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
}
// 头块与 trailer 块都搜 CRLFCRLF，且都靠 ud->prot_offset 续扫：半包到达时记下已扫过的字节数，
// 下次从它减 3 起搜——少 3 才能让跨两次读取的 CRLFCRLF 仍然命中。命中或出错都把它归零
static int32_t _http_search_crlf2(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t flens = CRLF_SIZE * 2;
    size_t start = ud->prot_offset > (flens - 1) ? ud->prot_offset - (flens - 1) : 0;
    int32_t pos = buffer_search(buf, 0, start, 0, CONCAT2(FLAG_CRLF,FLAG_CRLF), flens);
    if (ERR_FAILED == pos) {
        size_t bsize = buffer_size(buf);
        if (bsize > HTTP_MAX_HEADLENS) {
            BIT_SET(*status, PROT_ERROR);
            ud->prot_offset = 0;
        } else {
            BIT_SET(*status, PROT_MOREDATA);
            ud->prot_offset = bsize;
        }
        return ERR_FAILED;
    }
    ud->prot_offset = 0;
    return pos;
}
// 头部块总长（含结尾 CRLFCRLF）；返回 0 表示没解出来，等更多数据还是超 HTTP_MAX_HEADLENS 看 status
static size_t _http_headlens(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    int32_t pos = _http_search_crlf2(buf, ud, status);
    if (ERR_FAILED == pos) {
        return 0;
    }
    size_t hlens = (size_t)pos + CRLF_SIZE * 2;
    if (hlens > HTTP_MAX_HEADLENS) {
        BIT_SET(*status, PROT_ERROR);
        return 0;
    }
    return hlens;
}
// 分配 http_pack_ctx 结构体，头部数据紧随其后（连续内存），初始化头部字段数组。
// 只清结构体前缀：那 lens 字节紧接着就被 _http_parsehead 的 buffer_remove 整块写满，
// 连它一起清等于每请求白 memset 一个头块（上限 HTTP_MAX_HEADLENS）
static http_pack_ctx *_http_headpack(size_t lens) {
    char *pack;
    MALLOC(pack, sizeof(http_pack_ctx) + lens);
    ZERO(pack, sizeof(http_pack_ctx));
    ((http_pack_ctx *)pack)->head.data = pack + sizeof(http_pack_ctx);
    ((http_pack_ctx *)pack)->head.lens = lens;
    array_init(&((http_pack_ctx *)pack)->header, sizeof(http_header_ctx), 0);
    return (http_pack_ctx *)pack;
}
http_pack_ctx *_http_parsehead(buffer_ctx *buf, ud_cxt *ud, int32_t *transfer, int32_t *status) {
    size_t hlens = _http_headlens(buf, ud, status);
    if (0 == hlens) {
        return NULL;
    }
    *transfer = INIT;
    http_pack_ctx *pack = _http_headpack(hlens);
    ASSERTAB(hlens == buffer_remove(buf, pack->head.data, hlens), "copy buffer failed.");
    if (ERR_OK != _http_parse_head(pack, transfer)) {
        BIT_SET(*status, PROT_ERROR);
        _http_pkfree(pack);
        return NULL;
    }
    return pack;
}
// 解析 HTTP 头部后根据传输方式决定：直接返回（无数据体/chunked）或进入数据体读取。
// nobody 只由 ud->status 决定，故在此就地取——由调用方另传一个形参就是同一事实记两处
static http_pack_ctx *_http_header(buffer_ctx *buf, ud_cxt *ud, int32_t client, int32_t *status) {
    int32_t nobody = (INIT_NOBODY == ud->status) ? 1 : 0;
    int32_t transfer;
    http_pack_ctx *pack = _http_parsehead(buf, ud, &transfer, status);
    if (NULL == pack) {
        return NULL;
    }
    // 1xx/204/304 与 HEAD 的响应一律以头部后的空行结束，带了 CL/TE 也不算 body。
    // 不这么判，keep-alive 上会把下一条响应的头部吃成本条的 body
    if (0 != nobody
        || _http_nobody_resp(pack, client)) {
        pack->data.lens = 0;
        pack->chunked = 0;
        // 1xx 是中间响应,最终响应还在后面(RFC 7231 §6.2 要求客户端容忍任意条 1xx),
        // 此时不能把 HEAD 登记消耗掉,否则真正那条会被当成有 body
        ud->status = (0 != nobody && 0 != _http_interim_resp(pack, client)) ? INIT_NOBODY : INIT;
        return pack;
    }
    if (CONTENT == transfer) {
        if (pack->data.lens > HTTP_MAX_CONTENT_LENS) {
            BIT_SET(*status, PROT_ERROR);
            _http_pkfree(pack);
            return NULL;
        } else {
            ud->context = pack;
            ud->status = transfer;
            return _http_content(buf, ud, status);
        }
    } else {
        if (1 == pack->chunked) {
            BIT_SET(*status, PROT_SLICE_START);
        } else if (0 != client) {
            // 响应既无 Content-Length 又无 Transfer-Encoding：body 由连接关闭界定
            // (RFC 7230 §3.3.3 规则 7)。按分片投：本包是首片，body 逐段跟上，末片由
            // 关闭事件补(见 _http_on_close)。请求侧无此规则，无 CL/TE 即无 body
            BIT_SET(*status, PROT_SLICE_START);
            transfer = TILLCLOSE;
        }
        ud->status = transfer;
        return pack;
    }
}
// 分配 chunked 数据包结构体，lens>0 时数据紧随其后，chunked 字段固定设为 2。
// 只清结构体前缀，同 _http_headpack：载荷由 buffer_copyout 整块写满，而这里是每帧一次，
// 连载荷一起清就等于把整条流的字节数白 memset 一遍
static http_pack_ctx *_http_chunkedpack(size_t lens) {
    char *pack;
    MALLOC(pack, sizeof(http_pack_ctx) + lens);
    ZERO(pack, sizeof(http_pack_ctx));
    http_pack_ctx *pctx = (http_pack_ctx *)pack;
    if (lens > 0) {
        pctx->data.data = pack + sizeof(http_pack_ctx);
        pctx->data.lens = lens;
    }
    pctx->chunked = 2;
    return pctx;
}
// 解析 chunked 编码的数据块：先读取长度行，再读取对应数据，长度为 0 表示结束。
// trailer 块的搜索与 _http_headlens 共用 _http_search_crlf2（CHUNKED 期间不再调用后者，
// 两者不争用 prot_offset）；长度行只有 1~16 个 hex 字符，重扫可忽略，不续扫
static http_pack_ctx *_http_chunked(buffer_ctx *buf, ud_cxt *ud, int32_t *status) {
    size_t drain;
    http_pack_ctx *pack = ud->context;
    if (NULL == pack) {
        int32_t pos = buffer_search(buf, 0, 0, 0, FLAG_CRLF, CRLF_SIZE);
        if (pos < 0) {
            // 长度行还没收全时，缓冲里的字节全都属于这一行。没有上限的话，对端只要一直发
            // 不带 CRLF 的数据就能让接收缓冲无限涨，一条连接即可耗尽内存；头块与 trailer
            // 块都是按 HTTP_MAX_HEADLENS 这么挡的
            if (buffer_size(buf) > HTTP_MAX_HEADLENS) {
                BIT_SET(*status, PROT_ERROR);
            } else {
                BIT_SET(*status, PROT_MOREDATA);
            }
            return NULL;
        }
        if (0 == pos) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        // 整行长度按 HTTP_MAX_HEADLENS 卡（同头块与 trailer 块）。行内的 chunk-ext 多长都不影响
        // chunk-size 的解析，故只在这里卡总长，不拿它去限制下面那个栈缓冲
        if (pos > (int32_t)HTTP_MAX_HEADLENS) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        // RFC 7230 §4.1：chunk = chunk-size [ chunk-ext ] CRLF。只截 ';' 之前的 chunk-size，
        // ext 原样跳过：不能拿整行长度去卡下面那个 16 字节栈缓冲——带签名的 chunk-ext 有 80
        // 多字节、零填充的 chunk-size 又正好 16 字节，两者都是合法传输
        int32_t semi = buffer_search(buf, 0, 0, (size_t)pos, ";", 1);
        int32_t hexlens = (semi >= 0) ? semi : pos;
        char lensbuf[17] = { 0 };// 64 位十六进制最多 16 位 + NUL
        if (hexlens <= 0
            || hexlens >= (int32_t)sizeof(lensbuf)) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        ASSERTAB(hexlens == (int32_t)buffer_copyout(buf, 0, lensbuf, (size_t)hexlens), "copy buffer failed.");
        // RFC 7230 §4.1：chunk-size = 1*HEXDIG。首字符必须是 HEXDIG，否则 strtoul 会跳过前导空白、
        // 吞 '+'/'-'、或在空白后接受 "0x" 前缀，造成与上下游对 chunk 边界解析分歧（请求走私）
        unsigned char c0 = (unsigned char)lensbuf[0];
        if (!((c0 >= '0' && c0 <= '9') || (c0 >= 'a' && c0 <= 'f') || (c0 >= 'A' && c0 <= 'F'))) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        // 无前导空白的 "0x"/"0X"（首字符 '0' 已过上面校验）：strtoul base=16 会接受，显式拒绝
        if ('0' == lensbuf[0]
            && ('x' == lensbuf[1] || 'X' == lensbuf[1])) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        char *_endptr;
        errno = 0;
        size_t dlens = (size_t)strtoul(lensbuf, &_endptr, 16);
        // 截出来的这段必须整段都是 hex：_endptr 要停在 NUL 上（ext 已在上面切掉，不再放行 ';'）。
        // ERANGE 也判：溢出时 strtoul 返 ULONG_MAX，下面那道上限同样挡得住，这里只是把
        // "数值溢出"与"超上限"分成两种拒因
        if (_endptr == lensbuf
            || '\0' != *_endptr
            || ERANGE == errno) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        if (dlens > HTTP_MAX_CHUNK_LENS) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        drain = pos + CRLF_SIZE;
        ASSERTAB(drain == buffer_drain(buf, drain), "drain buffer failed.");
        pack = _http_chunkedpack(dlens);
        ud->context = pack;
    }
    if (pack->data.lens > 0) {
        drain = pack->data.lens + CRLF_SIZE;
        if (buffer_size(buf) < drain) {
            BIT_SET(*status, PROT_MOREDATA);
            return NULL;
        }
        if ('\r' != buffer_at(buf, pack->data.lens)
            || '\n' != buffer_at(buf, pack->data.lens + 1)) {
            BIT_SET(*status, PROT_ERROR);
            return NULL;
        }
        BIT_SET(*status, PROT_SLICE);
        ASSERTAB(pack->data.lens == buffer_copyout(buf, 0, pack->data.data, pack->data.lens), "copy buffer failed.");
    } else {
        // 末尾块：跳过可选 trailer headers + 终止空行（RFC 7230 §4.1）
        // 无 trailer: \r\n
        // 有 trailer: Content-MD5: xxx\r\n\r\n
        if (buffer_size(buf) < CRLF_SIZE) {
            BIT_SET(*status, PROT_MOREDATA);
            return NULL;
        }
        if ('\r' == buffer_at(buf, 0) && '\n' == buffer_at(buf, 1)) {
            drain = CRLF_SIZE;
        } else {
            // RFC 7230 §4.1.2 trailer headers 受 HTTP_MAX_HEADLENS=4KB 上限保护，
            // 防恶意 server 无限发 trailer 数据触发 buf 持续累积
            int32_t tend = _http_search_crlf2(buf, ud, status);
            if (ERR_FAILED == tend) {
                return NULL;
            }
            drain = (size_t)tend + CRLF_SIZE * 2;
            if (drain > HTTP_MAX_HEADLENS) {
                BIT_SET(*status, PROT_ERROR);
                return NULL;
            }
        }
        BIT_SET(*status, PROT_SLICE_END);
        ud->status = INIT;
    }
    ASSERTAB(drain == buffer_drain(buf, drain), "drain buffer failed.");
    ud->context = NULL;
    return pack;
}
// 由连接关闭界定 body(RFC 7230 §3.3.3 规则 7)：缓冲里现有的字节全都是 body，原样切一片投出去。
// 不在解析器里攒完整 body——那要无界累积并另配一个总长上限，而分片投递业务本来就在用(chunked)
static http_pack_ctx *_http_tillclose(buffer_ctx *buf, int32_t *status) {
    size_t lens = buffer_size(buf);
    if (0 == lens) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    http_pack_ctx *pack = _http_chunkedpack(lens);
    ASSERTAB(lens == buffer_remove(buf, pack->data.data, lens), "copy buffer failed.");
    BIT_SET(*status, PROT_SLICE);
    return pack;
}
void _http_pkfree(void *data) {
    http_pack_ctx *pack = (http_pack_ctx *)data;
    if (NULL == pack) {
        return;
    }
    if (NULL != pack->head.data) {
        FREE(pack->data.data);
        array_free(&pack->header);
    }
    FREE(pack);
}
void *_http_on_close(ud_cxt *ud) {
    if (TILLCLOSE != ud->status) {
        return NULL;
    }
    ud->status = INIT;
    return _http_chunkedpack(0);
}
void _http_udfree(ud_cxt *ud) {
    _http_pkfree(ud->context);
    ud->context = NULL;
}
void *http_unpack(struct ev_ctx *ev, SOCKET fd, uint64_t skid, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    (void)ev; (void)fd; (void)skid; (void)size;
    http_pack_ctx *pack;
    switch (ud->status) {
    case INIT:
    case INIT_NOBODY:
        pack = _http_header(buf, ud, client, status);
        break;
    case CONTENT:
        pack = _http_content(buf, ud, status);
        break;
    case CHUNKED:
        pack = _http_chunked(buf, ud, status);
        break;
    case TILLCLOSE:
        pack = _http_tillclose(buf, status);
        break;
    default:
        pack = NULL;
        BIT_SET(*status, PROT_ERROR);
        break;
    }
    return pack;
}
// chunked 的中间块与结束块只有数据、没有首行和头部（_http_chunkedpack 不设 head.data），
// 首行/头部四个访问器统一按这个判据返回空，别让调用方各自记得判一遍
buf_ctx *http_status(http_pack_ctx *pack) {
    return (NULL != pack->head.data) ? pack->status : NULL;
}
uint32_t http_nheader(http_pack_ctx *pack) {
    return (NULL != pack->head.data) ? array_size(&pack->header) : 0;
}
http_header_ctx *http_header_at(http_pack_ctx *pack, uint32_t pos) {
    return (NULL != pack->head.data) ? array_at(&pack->header, pos) : NULL;
}
char *http_header(http_pack_ctx *pack, const char *header, size_t *lens) {
    if (NULL == pack->head.data) {
        return NULL;
    }
    http_header_ctx *filed;
    size_t klens = strlen(header);
    uint32_t n = array_size(&pack->header);
    for (uint32_t i = 0; i < n; i++) {
        filed = array_at(&pack->header, i);
        if (buf_icompare(&filed->key, header, klens)) {
            *lens = filed->value.lens;
            return filed->value.data;
        }
    }
    return NULL;
}
int32_t http_chunked(http_pack_ctx *pack) {
    return pack->chunked;
}
void *http_data(http_pack_ctx *pack, size_t *lens) {
    *lens = pack->data.lens;
    return pack->data.data;
}
void http_pack_req(binary_ctx *bwriter, const char *method, const char *url) {
    ASSERTAB(NULL == strpbrk(method, "\r\n") && NULL == strpbrk(url, "\r\n"), "HTTP method/url must not contain CRLF.");
    binary_set_va(bwriter, "%s %s HTTP/1.1"FLAG_CRLF, method, url);
}
static int32_t _http_set_nobody_cb(struct watcher_ctx *watcher, struct sock_ctx *skctx,
    void *data, uint64_t number) {
    (void)watcher;
    (void)data;
    ud_cxt *ud = _evpub_get_ud(skctx);
    // ud->status 是各协议共用的解析状态字节,写到非 HTTP 连接上就是把别人的状态机踢乱。
    // 本接口收的是裸 fd(调用方可传任意 fd),故必须自己认协议,口径同 _prots_emit_close_tail
    if (PACK_HTTP != ud->pktype) {
        LOG_WARN("http set nobody on fd %d: not an http connection.", (int32_t)number);
        return 0;
    }
    // 只在"等下一条响应头"这个时刻有意义：正读某条响应的 body 时置位会把状态机踢乱
    if (INIT != ud->status) {
        LOG_WARN("http set nobody on fd %d while a message body is in progress.", (int32_t)number);
        return 0;
    }
    ud->status = INIT_NOBODY;
    return 0;
}
int32_t http_set_method(ev_ctx *ev, SOCKET fd, uint64_t skid, const char *method) {
    // 目前只有 HEAD 需要登记,其余方法是空操作,连命令都不投。
    // 方法按 RFC 7231 §4.1 区分大小写,故直接 strcmp
    if (0 != strcmp(method, "HEAD")) {
        return ERR_OK;
    }
    return ev_props(ev, fd, skid, _http_set_nobody_cb, NULL, NULL, (uint64_t)fd);
}
const char *http_code_status(int32_t code) {
    switch (code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Time-out";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Request Entity Too Large";
    case 414: return "Request-URI Too Large";
    case 415: return "Unsupported Media Type";
    case 416: return "Requested range not satisfiable";
    case 417: return "Expectation Failed";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Time-out";
    case 505: return "HTTP Version not supported";
    default:
        return "Unknown";
    }
}
void http_pack_resp(binary_ctx *bwriter, int32_t code) {
    binary_set_va(bwriter, "HTTP/1.1 %d %s"FLAG_CRLF, code, http_code_status(code));
}
void http_pack_head(binary_ctx *bwriter, const char *key, const char *val) {
    // 只是 head2 的 \0 结尾入口: 头的线格式与校验规则单点落在 head2, 免得两处各改一半
    http_pack_head2(bwriter, key, val, strlen(val));
}
// 头值有没有 CR / LF。逐字符挡而非只挡 "\r\n" 连对：孤立 LF 也被相当多的解析器当行终止符，
// 放过它等于给按长度传值的这一路留下头注入口子。空值(lens 为 0)恒合法
static int32_t _http_head_val_nocrlf(const char *val, size_t lens) {
    if (0 == lens) {
        return 1;
    }
    return NULL != val
        && NULL == memchr(val, '\r', lens)
        && NULL == memchr(val, '\n', lens);
}
int32_t http_head_val_ok(const char *val, size_t lens) {
    if (0 == lens) {
        return 1;
    }
    return 0 != _http_head_val_nocrlf(val, lens)
        && NULL == memchr(val, '\0', lens);
}
void http_pack_head2(binary_ctx *bwriter, const char *key, const char *val, size_t lens) {
    ASSERTAB(NULL == strpbrk(key, FLAG_CRLF) && 0 != _http_head_val_nocrlf(val, lens),
        "HTTP header key/val must not contain CRLF.");
    binary_set_va(bwriter, "%s: ", key);
    binary_set_binary(bwriter, val, lens);
    binary_set_binary(bwriter, FLAG_CRLF, CRLF_SIZE);
}
void http_pack_end(binary_ctx *bwriter) {
    binary_set_binary(bwriter, FLAG_CRLF, CRLF_SIZE);
}
void http_pack_content(binary_ctx *bwriter, void *data, size_t lens) {
    if (!EMPTYPTR(data, lens)) {
        binary_set_va(bwriter, "Content-Length: %zu"CONCAT2(FLAG_CRLF, FLAG_CRLF), lens);
        binary_set_binary(bwriter, data, lens);
    } else {
        binary_set_va(bwriter, "%s", "Content-Length: 0"CONCAT2(FLAG_CRLF, FLAG_CRLF));
    }
}
void http_pack_chunked(binary_ctx *bwriter, void *data, size_t lens) {
    if (bwriter->offset > 0){
        binary_set_va(bwriter, "Transfer-Encoding: Chunked"CONCAT2(FLAG_CRLF, FLAG_CRLF));
    }
    if (EMPTYPTR(data, lens)) {
        binary_set_va(bwriter, "0"FLAG_CRLF);
    } else {
        binary_set_va(bwriter, "%zx"FLAG_CRLF, lens);
        binary_set_binary(bwriter, data, lens);
    }
    binary_set_binary(bwriter, FLAG_CRLF, CRLF_SIZE);
}
