#ifndef SMTP_H_
#define SMTP_H_

#include "srey/spub.h"

typedef struct smtp_ctx {
    uint16_t port;
    int32_t authtype;
    int32_t status;
    struct evssl_ctx *evssl;
    task_ctx *task;
    SOCKET fd;
    uint64_t skid;
    char user[64];
    char psw[64];
    char ip[IP_LENS];
}smtp_ctx;

void _smtp_init(void *hspush);
void _smtp_udfree(ud_cxt *ud);
void _smtp_closed(ud_cxt *ud);

void smtp_init(smtp_ctx *smtp, const char *ip, uint16_t port, struct evssl_ctx *evssl, const char *user, const char *psw);
int32_t smtp_try_connect(task_ctx *task, smtp_ctx *smtp);
int32_t smtp_check_auth(smtp_ctx *smtp);
int32_t smtp_check_code(char *pack, const char *code);
int32_t smtp_check_ok(char *pack);

char *smtp_pack_reset(void);
char *smtp_pack_quit(void);
char *smtp_pack_ping(void);
char *smtp_pack_from(const char *from);
char *smtp_pack_rcpt(const char *rcpt);
char *smtp_pack_data(void);
char *smtp_pack_mail(const char *subject, const char *data);

void *smtp_unpack(ev_ctx *ev, SOCKET fd, uint64_t skid, buffer_ctx *buf, ud_cxt *ud, size_t *size, int32_t *status);

#endif//SMTP_H_
