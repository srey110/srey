#include "protocol/dns.h"
#include "utils/netaddr.h"

#define DNS_FLAG1_RD        0x01u // 期望递归（请求时设置）
#define DNS_FLAG1_TC        0x02u // 响应被截断（响应时可能设置，见 RFC 1035 §4.1.1）
#define DNS_FLAG2_RCODE(f)  ((f) & 0x0Fu) // 提取响应码
#define DNS_RCODE_NXDOMAIN 3 // 名字不存在
#define DNS_A    1 // IPv4 地址记录类型
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
    // 装不下就保持原值不动：截断出来的 IP 连不上任何东西，还不如让上一次的配置继续生效
    if (ERR_OK != safe_fill_str(_dns_ip, sizeof(_dns_ip), ip)) {
        LOG_ERROR("dns ip exceeds %zu bytes: %zu, keep the old one.", sizeof(_dns_ip) - 1, strlen(ip));
    }
}
const char *dns_get_ip(void) {
    return _dns_ip;
}
// 将点分格式域名编码为 DNS 报文中的标签格式（长度+内容序列），结尾补根标签 0。
// 末尾一个 '.'(FQDN 写法)照收；空标签(空串、开头的 '.'、连续的 "..")与超 63 字节的标签拒：
// 空标签编出来是长度 0，服务端会把它当根标签提前收尾，后面的字节全错位。
// 编出来是去掉结尾 '.' 后的长度 + 2(首个长度字节与根标签)，RFC 1035 §2.3.4 限 255 字节，故去点后最多 253
static int32_t _dns_encode_domain(char *qname, const char *domain, size_t *lenout) {
    char *qname_start = qname;
    size_t lock = 0;
    size_t dlens = strlen(domain);
    if (dlens > 0 && '.' == domain[dlens - 1]) {
        dlens--;
    }
    if (dlens > 253) {
        return ERR_FAILED;
    }
    for (size_t i = 0; i <= dlens; i++) {
        if (i < dlens && '.' != domain[i]) {
            continue;
        }
        if (i == lock
            || i - lock > 63) {
            return ERR_FAILED;
        }
        *qname++ = (char)(i - lock);
        memcpy(qname, domain + lock, i - lock);
        qname += i - lock;
        lock = i + 1;
    }
    *qname++ = '\0';
    *lenout = (size_t)(qname - qname_start);
    return ERR_OK;
}
size_t dns_request_pack(char *buf, const char *domain, int32_t ipv6, uint16_t *id) {
    dns_head head;
    uint16_t rid;
    if (ERR_OK != csprng_rand(&rid, sizeof(rid))) {
        return 0;
    }
    *id = rid;
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
size_t dns_request_pack_tcp(char *buf, const char *domain, int32_t ipv6, uint16_t *id) {
    size_t dlens = dns_request_pack(buf + 2, domain, ipv6, id);
    if (0 == dlens) {
        return 0;
    }
    write_be16(buf, (uint16_t)dlens);
    return dlens + sizeof(uint16_t);
}
void *dns_unpack(struct ev_ctx *ev, sock_ctx *sk, int32_t client,
    buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status) {
    (void)ev; (void)sk; (void)client; (void)ud;
    size_t avail = buffer_size(buf);
    if (avail < sizeof(uint16_t)) {
        BIT_SET(*status, PROT_MOREDATA);
        return NULL;
    }
    uint16_t nlen;
    ASSERTAB(sizeof(nlen) == buffer_copyout(buf, 0, &nlen, sizeof(nlen)), "copyout buffer error.");
    uint16_t plen = read_be16(&nlen);
    if (0 == plen) {
        BIT_SET(*status, PROT_ERROR);
        return NULL;
    }
    if (avail < (size_t)sizeof(nlen) + plen) {
        BIT_SET(*status, PROT_MOREDATA);
        buffer_want(buf, (size_t)sizeof(nlen) + plen - avail);
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
// 返回 ERR_OK 成功，ERR_FAILED 报文格式非法（越界/指针环路/name 溢出）。
// 每个标签(长度字节 + 内容)整段按一次拷贝：须整段落在报文内、装得进 name(末尾留 NUL)，内容里不许有 0 字节(按 C 串取名会被截断)
static int32_t _dns_decode_domain(unsigned char *name, size_t namelen,
                                  unsigned char *reader, unsigned char *buffer, size_t buflen,
                                  int32_t *count, uint32_t *jump_budget) {
    uint32_t p = 0, o = 0, jumped = 0, len;
    uint32_t offset;
    unsigned char *buf_end = buffer + buflen;
    *count = 1;
    name[0] = '\0';
    while (reader < buf_end
        && 0 != *reader) {
        if (*reader >= 192) {
            // 指针压缩：高两位为 11，后跟 14 位偏移
            if (reader + 1 >= buf_end) {
                return ERR_FAILED;
            }
            // 全报文共享跳转预算：单个报文内所有域名解压的指针跳转总数受限，
            // 既防单名指针环路，也防多记录共享长指针链的跨记录 O(n^2) CPU 放大
            if (0 == *jump_budget) {
                return ERR_FAILED;
            }
            (*jump_budget)--;
            offset = (uint32_t)((*reader & 0x3F) << 8) | *(reader + 1);
            if (offset >= buflen) {
                return ERR_FAILED;
            }
            if (0 == jumped) {
                (*count)++; // 计入指针第二字节
            }
            reader = buffer + offset;
            jumped = 1;
            continue;
        }
        if (*reader > 63) {
            return ERR_FAILED; // 高两位 01/10 为 RFC 1035 保留标签类型
        }
        len = *reader;
        if (p + len >= namelen - 1
            || (size_t)(buf_end - reader) < 1 + (size_t)len
            || NULL != memchr(reader + 1, 0, len)) {
            return ERR_FAILED;
        }
        if (0 != o) {
            name[o++] = '.';
        }
        memcpy(name + o, reader + 1, len);
        o += len;
        p += 1 + len;
        if (0 == jumped) {
            *count += (int32_t)(1 + len);
        }
        reader += 1 + len;
    }
    if (reader >= buf_end) {
        return ERR_FAILED;
    }
    name[o] = '\0';
    return ERR_OK;
}
// 解析应答段的 n 条资源记录，A/AAAA 的地址依次写进 dnsips，*cnt 输出写了几个(只在成功时写)。
// 返回应答段之后的 reader 位置，出错返回 NULL
static char *_dns_parse_data(char *buf, size_t buflen, char *reader, uint16_t n, dns_ip *dnsips, size_t *cnt) {
    char *buf_end = buf + buflen;
    uint32_t jump_budget = (uint32_t)buflen;// 全报文共享，见 _dns_decode_domain
    size_t index = 0;
    int32_t used;
    uint16_t rtype, rlens;
    char domain[256];
    for (uint16_t i = 0; i < n; i++) {
        if (ERR_OK != _dns_decode_domain((unsigned char *)domain, sizeof(domain),
                                     (unsigned char *)reader, (unsigned char *)buf, buflen, &used, &jump_budget)) {
            return NULL;
        }
        reader += used;
        // 严格校验：reader 必须在 [buf, buf_end] 内（允许 one-past-end），再用 buf_end-reader
        // 计算剩余字节，避免 reader + N 超越 one-past-end 的指针算术 UB（C11 §6.5.6）
        if (reader < buf || reader > buf_end) {
            return NULL;
        }
        // 校验固定字段（type + class + ttl + rdlength = 10 字节）是否在 buffer 内
        if ((size_t)(buf_end - reader) < 2 * sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint16_t)) {
            return NULL;
        }
        rtype = read_be16(reader);
        reader += 2 * sizeof(uint16_t) + sizeof(uint32_t); // 跳过 type/class/ttl
        rlens = read_be16(reader);
        reader += sizeof(uint16_t);
        // reader 已推进 10 字节，由前面 buf_end-reader >= 10 保证 reader <= buf_end，减法安全
        if ((size_t)(buf_end - reader) < rlens) {
            return NULL;
        }
        //地址转文本按地址族固定读 4 字节（IPv4）/ 16 字节（IPv6），
        //rlens 不严格相等会读到相邻记录或越界栈内存，恶意 DNS 响应可借此泄漏信息或返回伪造 IP
        if (DNS_A == rtype && 4 == rlens) {
            netaddr_ip4_str((const uint8_t *)reader, dnsips[index++].ip);
        } else if (DNS_AAAA == rtype && 16 == rlens) {
            inet_ntop(AF_INET6, reader, dnsips[index++].ip, sizeof(dnsips[0].ip));
        }
        reader += rlens;
    }
    *cnt = index;
    return reader;
}
dns_ip *dns_parse_pack(char *buf, size_t buflen, size_t *cnt, uint16_t id, int32_t *nodata) {
    // 入口统一归零：往下所有失败路径都不再单独写 cnt(写了也是无操作),
    // "返回 NULL 时 cnt 恒为 0"这条对外契约由这一句独家保证；
    // 唯一的非零写入是函数末尾成功路径的 *cnt = nip
    *cnt = 0;
    SET_PTR(nodata, 0);
    if (buflen < sizeof(dns_head)) {
        return NULL;
    }
    dns_head head;
    memcpy(&head, buf, sizeof(dns_head));
    if (ntohs(head.id) != id) {
        return NULL;
    }
    if (BIT_CHECK(head.flags1, DNS_FLAG1_TC)) {
        // 截断 提前返回 NULL
        return NULL;
    }
    uint8_t rcode = DNS_FLAG2_RCODE(head.flags2);
    if (0 != rcode) {
        // NXDOMAIN 是服务端对"名字不存在"的明确答复，换 TCP 重查还是同一结果；SERVFAIL 等可能是暂时故障，仍值得重试
        if (DNS_RCODE_NXDOMAIN == rcode) {
            SET_PTR(nodata, 1);
        }
        LOG_WARN("query domain error: %d.", rcode);
        return NULL;
    }
    // 只认应答段：授权段与附加段里的 A/AAAA 是名字服务器的地址(转介、glue)，不是这个名字的。
    // TC 与 rcode 都已查过，应答段空即 NOERROR/NODATA；下面那个 0 == total 是 maxrec 钳零，
    // 属"声称有记录却没字节"的畸形，不能当 NODATA
    uint16_t nans = ntohs(head.ans_count);
    if (0 == nans) {
        SET_PTR(nodata, 1);
        return NULL;
    }
    // 按 DNS label 格式安全跳过全部查询问题。边界一律用 end - p 的减法式, 理由见 _dns_parse_data
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
                if ((size_t)(end - p) < 2) {
                    return NULL;
                }
                p += 2;
                break;
            }
            if (llen >= 64) {
                return NULL; // RFC 1035 保留标签类型
            }
            if ((size_t)(end - p) < (size_t)1 + llen) {
                return NULL;
            }
            p += 1 + llen;
        }
        if ((size_t)(end - p) < sizeof(dns_question)) {
            return NULL;
        }
        p += sizeof(dns_question);
    }
    char *reader = p;
    // 按报文剩余字节推导记录数上界（每条 RR 最小 = NAME 1 字节 + 固定字段 10 字节），
    // 与应答段计数取小，防用极小报文 + 巨大计数字段放大分配
    size_t maxrec = (size_t)(end - reader) / 11;
    size_t total = nans;
    if (total > maxrec) {
        total = maxrec;
    }
    if (0 == total) {
        return NULL;
    }
    dns_ip *dnsips;
    MALLOC(dnsips, sizeof(dns_ip) * total);
    size_t nip = 0;
    // CNAME 链目标的 A/AAAA 属主名与查询名不同，故不按属主名过滤
    reader = _dns_parse_data(buf, buflen, reader, nans, dnsips, &nip);
    // nip==0 是报文合法但一条 A/AAAA 都没解出来。dnsips 是 MALLOC 的、一个字节都没写过，
    // 连同解析失败一起返 NULL，免得调用方按"非 NULL 即成功"读到未初始化内存。
    // 两者意义不同，用 nodata 区分：那是服务端的完整答复，换传输方式重查还是同一结果
    if (NULL == reader
        || 0 == nip) {
        if (NULL != reader) {
            SET_PTR(nodata, 1);
        }
        FREE(dnsips);
        return NULL;
    }
    *cnt = nip;
    return dnsips;
}
