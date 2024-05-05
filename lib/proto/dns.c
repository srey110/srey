#include "proto/dns.h"
#include "utils.h"

typedef struct dns_head  {
    uint16_t id;
    uint8_t rd : 1;
    uint8_t tc : 1;
    uint8_t aa : 1;
    uint8_t opcode : 4;
    uint8_t qr : 1;
    uint8_t rcode : 4;
    uint8_t cd : 1;
    uint8_t ad : 1;
    uint8_t z : 1;
    uint8_t ra : 1;
    uint16_t q_count;
    uint16_t ans_count;
    uint16_t auth_count;
    uint16_t add_count;
}dns_head;
typedef struct dns_question {
    uint16_t qtype;
    uint16_t qclass;
}dns_question;

#define DNS_A    1
#define DNS_AAAA 28

static void _encode_domain(char *qname, const char *domain) {
    size_t lock = 0, i, blens;
    char buf[256] = { 0 };
    memcpy(buf, domain, strlen(domain));
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
}
size_t dns_request_pack(char *buf, const char *domain, int32_t ipv6) {
    dns_head *head = (dns_head *)buf;
    head->id = (uint16_t)htons((u_short)(createid() % USHRT_MAX));
    head->rd = 1;
    head->q_count = htons(1);
    char *qname = &buf[sizeof(dns_head)];
    _encode_domain(qname, domain);
    size_t qlens = strlen(qname) + 1;
    dns_question *qinfo = (dns_question*)&buf[sizeof(dns_head) + qlens];
    if (0 == ipv6) {
        qinfo->qtype = htons(DNS_A);
    } else {
        qinfo->qtype = htons(DNS_AAAA);
    }
    qinfo->qclass = htons(1);
    return sizeof(dns_head) + qlens + sizeof(dns_question);
}
static void _decode_domain(unsigned char *name, unsigned char *reader, unsigned char *buffer, int32_t *count) {
    uint32_t p = 0, jumped = 0, offset;
    *count = 1;
    name[0] = '\0';
    while (*reader != 0) {
        if (*reader >= 192) {
            offset = (*reader) * 256 + *(reader + 1) - 49152;
            reader = buffer + offset - 1;
            jumped = 1;
        } else {
            name[p++] = *reader;
        }
        reader = reader + 1;
        if (0 == jumped) {
            (*count)++;
        }
    }
    name[p] = '\0';
    if (1 == jumped) {
        (*count)++;
    }
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
    name[i - 1] = '\0';
}
static char *_dns_parse_data(char *buf, char *reader, uint16_t n, dns_ip *dnsips, int32_t *index) {
    if (0 == n) {
        return reader;
    }
    int32_t cnt;
    dns_ip *tmp;
    uint16_t rtype, rlens;
    char domain[256];
    for (uint16_t i = 0; i < n; i++) {
        _decode_domain((unsigned char *)domain, (unsigned char *)reader, (unsigned char *)buf, &cnt);
        reader += cnt;
        rtype = ntohs(*((uint16_t *)reader));
        reader += 2 * sizeof(uint16_t) + sizeof(uint32_t);
        rlens = ntohs(*((uint16_t *)reader));
        reader += sizeof(uint16_t);
        if (DNS_A == rtype) {
            tmp = &dnsips[*index];
            (*index)++;
            inet_ntop(AF_INET, reader, tmp->ip, sizeof(tmp->ip));
        }else if (DNS_AAAA == rtype) {
            tmp = &dnsips[*index];
            (*index)++;
            inet_ntop(AF_INET6, reader, tmp->ip, sizeof(tmp->ip));
        } else {
            //_decode_domain((unsigned char *)domain, (unsigned char *)reader, (unsigned char *)buf, &cnt);
            //reader += cnt;
        }
        reader += rlens;
    }
    return reader;
}
dns_ip *dns_parse_pack(char *buf, size_t *cnt) {
    dns_head *head = (dns_head *)buf;
    if (0 != head->rcode) {
        LOG_WARN("qurey domain error: %d.", head->rcode);
        return NULL;
    }
    char *pname = &buf[sizeof(dns_head)];
    char *reader = &buf[sizeof(dns_head) + strlen(pname) + 1 + sizeof(dns_question)];
    uint16_t nans = ntohs(head->ans_count);
    uint16_t nauth = ntohs(head->auth_count);
    uint16_t nadd = ntohs(head->add_count);
    dns_ip *dnsips;
    MALLOC(dnsips, sizeof(dns_ip) * (nans + nauth + nadd));
    int32_t index = 0;
    //Answers
    reader = _dns_parse_data(buf, reader, nans, dnsips, &index);
    //Authoritative
    reader = _dns_parse_data(buf, reader, nauth, dnsips, &index);
    //Additional
    reader = _dns_parse_data(buf, reader, nadd, dnsips, &index);
    *cnt = (size_t)index;
    return dnsips;
}
