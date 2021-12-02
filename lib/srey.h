#ifndef SREY_H_
#define SREY_H_

#include "netev/netev.h"

#define MSG_TYPE_UNREG       0x01    //注销  
#define MSG_TYPE_TIMEOUT     0x02    //超时    itype   uisess
#define MSG_TYPE_ACCEPT      0x03    //itype   pmsg(struct sock_ctx *)
#define MSG_TYPE_CONNECT     0x04    //itype   uisess  pmsg(struct sock_ctx *) uisize(error)
#define MSG_TYPE_RECV        0x05    //itype   pmsg(struct sock_ctx *)  uisize
#define MSG_TYPE_RECVFROM    0x06    //itype   pmsg(struct udp_recv_msg *) uisize
#define MSG_TYPE_SEND        0x07    //itype   pmsg(struct sock_ctx *)  uisize
#define MSG_TYPE_CLOSE       0x08    //itype   pmsg(struct sock_ctx *)
#define MSG_TYPE_REQUEST     0x09    //itype   srcid uisess  pmsg uisize
#define MSG_TYPE_RESPONSE    0x0A    //itype   uisess  pmsg uisize

struct task_ctx;
typedef void(*module_run)(struct task_ctx *ptask, uint32_t itype, sid_t srcid, uint32_t uisess, void *pmsg, uint32_t uisize, void *pud);
typedef void *(*module_create)(struct task_ctx *ptask, void *pud);
typedef int32_t(*module_init)(struct task_ctx *ptask, void *pmd, void *pud);
typedef void(*module_release)(struct task_ctx *ptask, void *pmd, void *pud);
typedef void(*module_msg_release)(void *pmsg);//任务间消息释放
struct module_ctx
{
    uint32_t maxcnt;//每次最多执行命令数
    module_create create;//创建
    module_init init;//初始化
    module_release release;//释放
    module_run run;//消息处理
    char name[NAME_LENS];//任务名
};
struct udp_recv_msg
{
    uint16_t port;
    struct sock_ctx *sock;
    char ip[IP_LENS];
};
/*
* \brief          初始化
* \param uiworker 工作线程数  0 核心数 * 2
* \param msgfree  任务间通信消息释放函数
*/
struct srey_ctx *srey_new(uint32_t uiworker, module_msg_release msgfree);
/*
* \brief          释放
*/
void srey_free(struct srey_ctx *pctx);
/*
* \brief          运行
*/
void srey_loop(struct srey_ctx *pctx);
/*
* \brief          任务注册
* \param pmodule  struct module_ctx
* \param pudata   用户数据
* \return         0 失败
* \return         任务ID
*/
sid_t srey_register(struct srey_ctx *pctx, struct module_ctx *pmodule, void *pudata);
/*
* \brief          任务ID查询
* \param pname    任务名
* \return         0 无
* \return         任务ID
*/
sid_t srey_queryid(struct srey_ctx *pctx, const char *pname);
/*
* \brief          所有任务ID查询
* \param pqu      struct queue_ctx  id
*/
void srey_allid(struct srey_ctx *pctx, struct queue_ctx *pqu);
/*
* \brief          任务注销
* \param id       任务id
*/
void srey_unregister(struct srey_ctx *pctx, sid_t id);
/*
* \brief          向任务(toid/pname)发送消息，该消息无返回
* \param toid     接收消息的任务ID
* \param pmsg     消息
* \param uisz     消息长度
* \return         ERR_OK 成功
* \return         其他 失败
*/
int32_t srey_callid(struct srey_ctx *pctx, sid_t toid, void *pmsg, uint32_t uisz);
int32_t srey_callnam(struct srey_ctx *pctx, const char *pname, void *pmsg, uint32_t uisz);
/*
* \brief          向任务(toid/pname)发送消息
* \param toid     接收消息的任务ID
* \param srcid    发起该消息的任务ID
* \param uisess   session 标识
* \param pmsg     消息
* \param uisz     消息长度
* \return         ERR_OK 成功
* \return         其他 失败
*/
int32_t srey_reqid(struct srey_ctx *pctx, sid_t toid, 
    sid_t srcid, uint32_t uisess, void *pmsg, uint32_t uisz);
int32_t srey_reqnam(struct srey_ctx *pctx, const char *pname, 
    sid_t srcid, uint32_t uisess, void *pmsg, uint32_t uisz);
/*
* \brief          返回srey_req请求
* \param toid     接收消息的任务ID
* \param uisess   session 标识
* \param pmsg     消息
* \param uisz     消息长度
* \return         ERR_OK 成功
* \return         其他 失败
*/
int32_t srey_response(struct srey_ctx *pctx, sid_t toid, 
    uint32_t uisess, void *pmsg, uint32_t uisz);
/*
* \brief           注册超时
* \param ownerid   绑定的任务ID
* \param uisess    session 标识
* \param uitimeout 超时时间
*/
void srey_timeout(struct srey_ctx *pctx, sid_t ownerid, uint32_t uisess, uint32_t uitimeout);
/*
* \brief           监听
* \param ownerid   绑定的任务ID
* \param phost     ip
* \param usport    port
* \return          NULL 失败
* \return          struct listener_ctx
*/
struct listener_ctx *srey_listener(struct srey_ctx *pctx, sid_t ownerid, 
    const char *phost, uint16_t usport);
/*
* \brief           链接
* \param ownerid   绑定的任务ID
* \param uisess    session 标识
* \param utimeout  超时
* \param phost     ip
* \param usport    port
* \return          NULL 失败
* \return          struct sock_ctx
*/
struct sock_ctx *srey_connecter(struct srey_ctx *pctx, sid_t ownerid, uint32_t uisess,
    uint32_t utimeout, const char *phost, uint16_t usport);
/*
* \brief           添加自定义的socket
* \param sock      socket
* \param itype     SOCK_STREAM  SOCK_DGRAM
* \param ifamily   AF_INET  AF_INET6
* \return          NULL 失败
* \return          struct sock_ctx
*/
struct sock_ctx *srey_addsock(struct srey_ctx *pctx, SOCKET sock, int32_t itype, int32_t ifamily);
/*
* \brief           开始读写
* \param ownerid   绑定的任务ID
* \param psock     struct sock_ctx
* \param iwrite    是否需要MSG_TYPE_SEND消息  0 否
* \return          ERR_OK 成功
* \return          其他 失败
*/
int32_t srey_enable(struct srey_ctx *pctx, sid_t ownerid, struct sock_ctx *psock, int32_t iwrite);
/*
* \brief           获取一session
* \return          session
*/
uint32_t task_new_session(struct task_ctx *ptask);
/*
* \brief           任务ID
* \return          ID
*/
sid_t task_id(struct task_ctx *ptask);
/*
* \brief           任务名
* \return          任务名
*/
const char *task_name(struct task_ctx *ptask);

#endif//SREY_H_
