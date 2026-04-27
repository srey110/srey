#include "protocol/dns.h"
#include "utils/utils.h"

// DNS 报文头部结构
typedef struct dns_head {
    uint16_t id;         // 事务 ID
    uint8_t rd : 1;      // 期望递归
    uint8_t tc : 1;      // 截断标志
    uint8_t aa : 1;      // 权威应答
    uint8_t opcode : 4;  // 操作码
    uint8_t qr : 1;      // 0=查询 1=响应
    uint8_t rcode : 4;   // 响应码
    uint8_t cd : 1;      // 禁用验证
    uint8_t ad : 1;      // 已验证数据
    uint8_t z : 1;       // 保留位
    uint8_t ra : 1;      // 可用递归
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

#define DNS_A    1  // IPv4 地址记录类型
#define DNS_AAAA 28 // IPv6 地址记录类型
static char _dns_ip[IP_LENS]; // 全局 DNS 服务器 IP 地址

void dns_set_ip(const char *ip) {
    ZERO(_dns_ip, sizeof(_dns_ip));
    strncpy(_dns_ip, ip, sizeof(_dns_ip) - 1);
}
const char *dns_get_ip(void) {
    return _dns_ip;
}
// 将点分格式域名编码为 DNS 报文中的标签格式（长度+内容序列）
static int32_t _encode_domain(char *qname, const char *domain) {
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
            *qname++ = (char)(i - lock);
            for (; lock < i; lock++) {
                *qname++ = buf[lock];
            }
            lock++;
        }
    }
    *qname++ = '\0';
    return ERR_OK;
}
size_t dns_request_pack(char *buf, const char *domain, int32_t ipv6) {
    dns_head *head = (dns_head *)buf;
    head->id = (uint16_t)htons((u_short)(createid() % USHRT_MAX));
    head->rd = 1;         // 请求递归查询
    head->q_count = htons(1);
    char *qname = &buf[sizeof(dns_head)];
    if (ERR_OK != _encode_domain(qname, domain)) {
        return 0;
    }
    size_t qlens = strlen(qname) + 1;
    dns_question *qinfo = (dns_question*)&buf[sizeof(dns_head) + qlens];
    if (0 == ipv6) {
        qinfo->qtype = htons(DNS_A);    // 查询 IPv4
    } else {
        qinfo->qtype = htons(DNS_AAAA); // 查询 IPv6
    }
    qinfo->qclass = htons(1); // IN 类（互联网）
    return sizeof(dns_head) + qlens + sizeof(dns_question);
}
// 将 DNS 报文中的标签格式域名解码为点分格式，count 输出已消耗的字节数
// 返回 ERR_OK 成功，ERR_FAILED 报文格式非法（越界/指针环路/name 溢出）
static int32_t _decode_domain(unsigned char *name, size_t namelen,
                               unsigned char *reader, unsigned char *buffer, size_t buflen,
                               int32_t *count) {
    uint32_t p = 0, jumped = 0, jump_count = 0;
    const uint32_t MAX_JUMPS = 10;
    unsigned char *buf_end = buffer + buflen;
    *count = 1;
    name[0] = '\0';
    while (reader < buf_end && *reader != 0) {
        if (*reader >= 192) {
            // 指针压缩：高两位为 11，后跟 14 位偏移
            if (reader + 1 >= buf_end) {
                return ERR_FAILED;
            }
            if (jump_count++ >= MAX_JUMPS) {
                return ERR_FAILED; // 防止压缩指针构成环路
            }
            uint32_t offset = (uint32_t)((*reader & 0x3F) << 8) | *(reader + 1);
            if (offset >= buflen) {
                return ERR_FAILED;
            }
            if (0 == jumped) {
                (*count)++; // 计入指针第二字节
            }
            reader = buffer + offset;
            jumped = 1;
        } else {
            if (p >= namelen - 1) {
                return ERR_FAILED; // name 缓冲区已满
            }
            name[p++] = *reader;
            reader++;
            if (0 == jumped) {
                (*count)++;
            }
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
        if (ERR_OK != _decode_domain((unsigned char *)domain, sizeof(domain),
                                     (unsigned char *)reader, (unsigned char *)buf, buflen, &cnt)) {
            return NULL;
        }
        reader += cnt;
        // 校验固定字段（type + class + ttl + rdlength = 10 字节）是否在 buffer 内
        if (reader + 2 * sizeof(uint16_t) + sizeof(uint32_t) + sizeof(uint16_t) > buf_end) {
            return NULL;
        }
        uint16_t _u16;
        memcpy(&_u16, reader, sizeof(_u16));//直接转型解引用在 ARM、SPARC 等严格对齐平台上是未定义行为
        rtype = ntohs(_u16);
        reader += 2 * sizeof(uint16_t) + sizeof(uint32_t); // 跳过 type/class/ttl
        memcpy(&_u16, reader, sizeof(_u16));
        rlens = ntohs(_u16);
        reader += sizeof(uint16_t);
        if (reader + rlens > buf_end) {
            return NULL;
        }
        if (DNS_A == rtype) {
            tmp = &dnsips[*index];
            (*index)++;
            inet_ntop(AF_INET, reader, tmp->ip, sizeof(tmp->ip));
        } else if (DNS_AAAA == rtype) {
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
    dns_head *head = (dns_head *)buf;
    if (0 != head->rcode) {
        LOG_WARN("qurey domain error: %d.", head->rcode);
        return NULL;
    }
    uint16_t nans = ntohs(head->ans_count);
    uint16_t nauth = ntohs(head->auth_count);
    uint16_t nadd = ntohs(head->add_count);
    uint32_t total = (uint32_t)nans + nauth + nadd;
    if (0 == total) {
        *cnt = 0;
        return NULL;
    }
    char *pname = &buf[sizeof(dns_head)];
    // 跳过查询问题部分（域名 + 类型/类字段）
    char *reader = &buf[sizeof(dns_head) + strlen(pname) + 1 + sizeof(dns_question)];
    if (reader >= buf + buflen) {
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
