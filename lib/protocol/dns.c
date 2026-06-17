#include "protocol/dns.h"
#include "protocol/prots.h"
#include "base/config.h"
#include "utils/utils.h"

#define DNS_FLAG1_RD        0x01u   // 期望递归（请求时设置）
#define DNS_FLAG2_RCODE(f)  ((f) & 0x0Fu) // 提取响应码
#define DNS_A    1  // IPv4 地址记录类型
#define DNS_AAAA 28 // IPv6 地址记录类型

// DNS 报文头部结构（RFC 1035 §4.1.1，wire 字节序：网络字节序）
// flags1: QR(7) | OPCODE(6-3) | AA(2) | TC(1) | RD(0)
// flags2: RA(7) | Z(6)        | AD(5) | CD(4) | RCODE(3-0)
typedef struct dns_head {
    uint16_t id;         // 事务 ID
    uint8_t  flags1;
    uint8_t  flags2;
    uint16_t q_count;    // 查询问题数
    uint16_t ans_count;  // 应答记录数
    uint16_t auth_count; // 授权记录数
    uint16_t add_count;  // 附加记录数
}dns_head;
// DNS 查询问题结构
typedef struct dns_question {
    uint16_t qtype;  // 查询类型（A=1, AAAA=28）
    uint16_t qclass; // 查询类（互联网=1）
}dns_question;

static char _dns_ip[IP_LENS]; // 全局 DNS 服务器 IP 地址

void dns_set_ip(const char *ip) {
    safe_fill_str(_dns_ip, sizeof(_dns_ip), ip);
}
const char *dns_get_ip(void) {
    return _dns_ip;
}
// 将点分格式域名编码为 DNS 报文中的标签格式（长度+内容序列）
static int32_t _dns_encode_domain(char *qname, const char *domain, size_t *lenout) {
    char *qname_start = qname;
    size_t lock = 0, i, blens;
    char buf[256] = { 0 };
    size_t dlens = strlen(domain);
    if (dlens >= sizeof(buf) - 1) {
        return ERR_FAILED;
    }
    memcpy(buf, domain, dlens);
    strcat(buf, ".");
    blens = strlen(buf);
    for (i = 0; i < blens; i++) {
        if (buf[i] == '.') {
            if (i - lock > 63) {
                return ERR_FAILED;
            }
            *qname++ = (char)(i - lock);
            for (; lock < i; lock++) {
                *qname++ = buf[lock];
            }
            lock++;
        }
    }
    *qname++ = '\0';
    *lenout = (size_t)(qname - qname_start);
    return ERR_OK;
}
size_t dns_request_pack(char *buf, const char *domain, int32_t ipv6) {
    dns_head head;
    uint16_t rid;
    csprng_rand(&rid, sizeof(rid));
    head.id = (uint16_t)htons(rid);
    head.flags1 = DNS_FLAG1_RD;
    head.flags2 = 0;
    head.q_count = htons(1);
    head.ans_count = 0;
    head.auth_count = 0;
    head.add_count = 0;
    memcpy(buf, &head, sizeof(dns_head));
    char *qname = &buf[sizeof(dns_head)];
    size_t qlens;
    if (ERR_OK != _dns_encode_domain(qname, domain, &qlens)) {
        return 0;
    }
    dns_question qinfo;
    qinfo.qtype = (0 == ipv6) ? htons(DNS_A) : htons(DNS_AAAA);
    qinfo.qclass = htons(1);
    memcpy(&buf[sizeof(dns_head) + qlens], &qinfo, sizeof(dns_question));
    return sizeof(dns_head) + qlens + sizeof(dns_question);
}
size_t dns_request_pack_tcp(char *buf, const char *domain, int32_t ipv6) {
    size_t dlens = dns_request_pack(buf + 2, domain, ipv6);
    if (0 == dlens) {
        return 0;
    }
    uint16_t nlen = htons((uint16_t)dlens);
    memcpy(buf, &nlen, sizeof(nlen));
    return dlens + sizeof(nlen);
}
void *dns_unpack(buffer_ctx *buf, size_t *size, int32_t *status) {
    size_t avail = buffer_size(buf);
    if (avail < sizeof(uint16_t)) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    uint16_t nlen;
    ASSERTAB(sizeof(nlen) == buffer_copyout(buf, 0, &nlen, sizeof(nlen)), "copyout buffer error.");
    uint16_t plen = ntohs(nlen);
    if (0 == plen
        || PACK_TOO_LONG(plen)) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    if (avail < (size_t)sizeof(nlen) + plen) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    ASSERTAB(sizeof(nlen) == buffer_drain(buf, sizeof(nlen)), "drain buffer error.");
    void *pkt;
    MALLOC(pkt, plen);
    ASSERTAB(plen == buffer_remove(buf, pkt, plen), "remove buffer error.");
    *size = plen;
    return pkt;
}
// 将 DNS 报文中的标签格式域名解码为点分格式，count 输出已消耗的字节数
// 返回 ERR_OK 成功，ERR_FAILED 报文格式非法（越界/指针环路/name 溢出）
static int32_t _dns_decode_domain(unsigned char *name, size_t namelen,
                                  unsigned char *reader, unsigned char *buffer, size_t buflen,
                                  int32_t *count) {
    uint32_t p = 0, jumped = 0, jump_count = 0, label_remaining = 0;
    unsigned char *buf_end = buffer + buflen;
    *count = 1;
    name[0] = '\0';
    while (reader < buf_end && *reader != 0) {
        if (label_remaining > 0) {
            // 标签内容字节，直接拷入
            if (p >= namelen - 1) {
                return ERR_FAILED;
            }
            name[p++] = *reader;
            reader++;
            label_remaining--;
            if (0 == jumped) {
                (*count)++;
            }
        } else if (*reader >= 192) {
            // 指针压缩：高两位为 11，后跟 14 位偏移
            if (reader + 1 >= buf_end) {
                return ERR_FAILED;
            }
            // 每个压缩指针至少 2 字节，jump 数学上限 = buflen / 2，防环路而不误拒深度压缩
            if (jump_count++ >= buflen / 2) {
                return ERR_FAILED;
            }
            uint32_t offset = (uint32_t)((*reader & 0x3F) << 8) | *(reader + 1);
            if (offset >= buflen) {
                return ERR_FAILED;
            }
            if (0 == jumped) {
                (*count)++; // 计入指针第二字节
            }
            reader = buffer + offset;
            // 跳转后验证 reader 不超出缓冲区
            if (reader >= buf_end) {
                return ERR_FAILED;
            }
            jumped = 1;
        } else if (*reader <= 63) {
            // 标签长度字节
            label_remaining = *reader;
            if (p >= namelen - 1) {
                return ERR_FAILED;
            }
            name[p++] = *reader;
            reader++;
            if (0 == jumped) {
                (*count)++;
            }
        } else {
            return ERR_FAILED; // 高两位 01/10 为 RFC 1035 保留标签类型
        }
    }
    if (reader >= buf_end) {
        return ERR_FAILED;
    }
    name[p] = '\0';
    // 将标签格式转换为点分格式
    int32_t i, j;
    size_t nlens = strlen((const char*)name);
    for (i = 0; i < (int32_t)nlens; i++) {
        p = name[i];
        if (i + (int32_t)p >= (int32_t)nlens) {
            return ERR_FAILED;
        }
        for (j = 0; j < (int32_t)p; j++) {
            name[i] = name[i + 1];
            i++;
        }
        name[i] = '.';
    }
    if (i > 0) {
        name[i - 1] = '\0';
    }
    return ERR_OK;
}
// 解析 DNS 响应中的资源记录段（应答/授权/附加），提取 A/AAAA 类型的 IP 地址
// 返回下一个 reader 位置，出错返回 NULL
static char *_dns_parse_data(char *buf, size_t buflen, char *reader, uint16_t n, dns_ip *dnsips, int32_t *index) {
    if (0 == n) {
        return reader;
    }
    char *buf_end = buf + buflen;
    int32_t cnt;
    dns_ip *tmp;
    uint16_t rtype, rlens;
    char domain[256];
    for (uint16_t i = 0; i < n; i++) {
        if (ERR_OK != _dns_decode_domain((unsigned char *)domain, sizeof(domain),
                                     (unsigned char *)reader, (unsigned char *)buf, buflen, &cnt)) {
            return NULL;
        }
        reader += cnt;
        // 严格校验：reader 必须在 [buf, buf_end] 内（允许 one-past-end），再用 buf_end-reader
        // 计算剩余字节，避免 reader + N 超越 one-past-end 的指针算术 UB（C11 §6.5.6）
        if (reader < buf || reader > buf_end) {
            return NULL;
        }
        // 校验固定字段（type + class + ttl + rdlength = 10 字节）是否在 buffer 内
        if ((size_t)(buf_end - reader) < 2 * sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint16_t)) {
            return NULL;
        }
        uint16_t _u16;
        memcpy(&_u16, reader, sizeof(_u16));//直接转型解引用在 ARM、SPARC 等严格对齐平台上是未定义行为
        rtype = ntohs(_u16);
        reader += 2 * sizeof(uint16_t) + sizeof(uint32_t); // 跳过 type/class/ttl
        memcpy(&_u16, reader, sizeof(_u16));
        rlens = ntohs(_u16);
        reader += sizeof(uint16_t);
        // reader 已推进 10 字节，由前面 buf_end-reader >= 10 保证 reader <= buf_end，减法安全
        if ((size_t)(buf_end - reader) < rlens) {
            return NULL;
        }
        //inet_ntop 按地址族固定读 4 字节（IPv4）/ 16 字节（IPv6），
        //rlens 不严格相等会读到相邻记录或越界栈内存，恶意 DNS 响应可借此泄漏信息或返回伪造 IP
        if (DNS_A == rtype && 4 == rlens) {
            tmp = &dnsips[*index];
            (*index)++;
            inet_ntop(AF_INET, reader, tmp->ip, sizeof(tmp->ip));
        } else if (DNS_AAAA == rtype && 16 == rlens) {
            tmp = &dnsips[*index];
            (*index)++;
            inet_ntop(AF_INET6, reader, tmp->ip, sizeof(tmp->ip));
        }
        reader += rlens;
    }
    return reader;
}
dns_ip *dns_parse_pack(char *buf, size_t buflen, size_t *cnt) {
    if (buflen < sizeof(dns_head)) {
        return NULL;
    }
    dns_head head;
    memcpy(&head, buf, sizeof(dns_head));
    uint8_t rcode = DNS_FLAG2_RCODE(head.flags2);
    if (0 != rcode) {
        LOG_WARN("qurey domain error: %d.", rcode);
        return NULL;
    }
    uint16_t nans = ntohs(head.ans_count);
    uint16_t nauth = ntohs(head.auth_count);
    uint16_t nadd = ntohs(head.add_count);
    uint32_t total = (uint32_t)nans + nauth + nadd;
    if (0 == total) {
        *cnt = 0;
        return NULL;
    }
    /* 按 DNS label 格式安全跳过全部查询问题 */
    uint16_t nq = ntohs(head.q_count);
    char *end = buf + buflen;
    char *p = buf + sizeof(dns_head);
    uint8_t llen;
    for (uint16_t qi = 0; qi < nq; qi++) {
        for (;;) {
            if (p >= end) {
                return NULL;
            }
            llen = (uint8_t)*p;
            if (0x00 == llen) {
                p++;
                break;
            }
            if (0xC0 == (llen & 0xC0)) {
                if (p + 2 > end) {
                    return NULL;
                }
                p += 2;
                break;
            }
            if (llen >= 64) {
                return NULL; // RFC 1035 保留标签类型
            }
            if (p + 1 + llen > end) {
                return NULL;
            }
            p += 1 + llen;
        }
        if (p + (ptrdiff_t)sizeof(dns_question) > end) {
            return NULL;
        }
        p += sizeof(dns_question);
    }
    char *reader = p;
    // 按报文剩余字节推导记录数上界（每条 RR 最小 = NAME 1 字节 + 固定字段 10 字节），
    // 与头部计数取小，防用极小报文 + 巨大计数字段放大分配
    size_t maxrec = (size_t)(end - reader) / 11;
    if (total > maxrec) {
        total = (uint32_t)maxrec;
    }
    if (0 == total) {
        *cnt = 0;
        return NULL;
    }
    dns_ip *dnsips;
    MALLOC(dnsips, sizeof(dns_ip) * total);
    int32_t index = 0;
    // 解析应答段
    reader = _dns_parse_data(buf, buflen, reader, nans, dnsips, &index);
    // 解析授权段
    if (NULL != reader) {
        reader = _dns_parse_data(buf, buflen, reader, nauth, dnsips, &index);
    }
    // 解析附加段
    if (NULL != reader) {
        reader = _dns_parse_data(buf, buflen, reader, nadd, dnsips, &index);
    }
    if (NULL == reader) {
        FREE(dnsips);
        return NULL;
    }
    *cnt = (size_t)index;
    return dnsips;
}
