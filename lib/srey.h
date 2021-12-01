#ifndef SREY_H_
#define SREY_H_

#include "netev/netev.h"

struct task_ctx;
typedef void(*module_run)(struct task_ctx *ptask, uint32_t itype, sid_t srcid, uint32_t uisess, void *pmsg, uint32_t uisize, void *pud);
typedef void *(*module_create)(struct task_ctx *ptask, void *pud);
typedef int32_t(*module_init)(struct task_ctx *ptask, void *pmd, void *pud);
typedef void(*module_release)(struct task_ctx *ptask, void *pmd, void *pud);
typedef void(*module_msg_release)(void *pmsg);//任务间消息释放
struct module_ctx
{
    uint32_t maxcnt;//每次最多执行命令数
    module_create create;
    module_init init;
    module_release release;
    module_run run;
    char name[NAME_LENS];
};
struct udp_recv_msg
{
    uint16_t port;
    struct sock_ctx *sock;
    char ip[IP_LENS];
};
#define MSG_TYPE_UNREG       0x01    
#define MSG_TYPE_TIMEOUT     0x02    //itype   uisess
#define MSG_TYPE_ACCEPT      0x03    //itype   pmsg(struct sock_ctx *)
#define MSG_TYPE_CONNECT     0x04    //itype   uisess  pmsg(struct sock_ctx *) uisize(error)
#define MSG_TYPE_RECV        0x05    //itype   pmsg(struct sock_ctx *)  uisize
#define MSG_TYPE_RECVFROM    0x06    //itype   pmsg(struct udp_recv_msg *) uisize
#define MSG_TYPE_SEND        0x07    //itype   pmsg(struct sock_ctx *)  uisize
#define MSG_TYPE_CLOSE       0x08    //itype   pmsg(struct sock_ctx *)
#define MSG_TYPE_CALL        0x09    //itype   pmsg  uisize
#define MSG_TYPE_REQUEST     0x0A    //itype   srcid uisess  pmsg uisize
#define MSG_TYPE_RESPONSE    0x0B    //itype   uisess  pmsg uisize
#define MSG_TYPE_BROADCAST   0x0C    //itype   pmsg uisize

struct srey_ctx *srey_new(uint32_t uiworker, module_msg_release msgfree);
void srey_free(struct srey_ctx *pctx);
void srey_loop(struct srey_ctx *pctx);

sid_t srey_register(struct srey_ctx *pctx, struct module_ctx *pmodule, void *pudata);
sid_t srey_queryid(struct srey_ctx *pctx, const char *pname);
void srey_allid(struct srey_ctx *pctx, struct queue_ctx *pqu);
void srey_unregister(struct srey_ctx *pctx, sid_t id);

int32_t srey_call(struct srey_ctx *pctx, sid_t toid, void *pmsg, uint32_t uisz);
int32_t srey_request(struct srey_ctx *pctx, sid_t toid, 
    sid_t srcid, uint32_t uisess, void *pmsg, uint32_t uisz);
int32_t srey_response(struct srey_ctx *pctx, sid_t toid, 
    uint32_t uisess, void *pmsg, uint32_t uisz);
void srey_broadcast(struct srey_ctx *pctx, void *pmsg, uint32_t uisz);

void srey_timeout(struct srey_ctx *pctx, sid_t ownerid, uint32_t uisess, const uint32_t uitimeout);
struct listener_ctx *srey_listener(struct srey_ctx *pctx, sid_t ownerid, 
    const char *phost, const uint16_t usport);
struct sock_ctx *srey_connecter(struct srey_ctx *pctx, sid_t ownerid, uint32_t uisess,
    uint32_t utimeout, const char *phost, const uint16_t usport);
struct sock_ctx *srey_addsock(struct srey_ctx *pctx, SOCKET sock, int32_t itype, int32_t ifamily);
int32_t srey_enable(struct srey_ctx *pctx, sid_t ownerid, struct sock_ctx *psock, int32_t iwrite);

uint32_t task_new_session(struct task_ctx *ptask);
sid_t task_id(struct task_ctx *ptask);
const char *task_name(struct task_ctx *ptask);

#endif//SREY_H_
