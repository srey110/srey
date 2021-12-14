#ifndef SREY_H_
#define SREY_H_

#include "netev/netev.h"

#define MSG_TYPE_INIT        0x01
#define MSG_TYPE_STOP        0x02  
#define MSG_TYPE_FREE        0x03
#define MSG_TYPE_TIMEOUT     0x04    //超时    itype   uisess
#define MSG_TYPE_ACCEPT      0x05    //itype   pmsg(struct sock_ctx *)
#define MSG_TYPE_CONNECT     0x06    //itype   uisess  pmsg(struct sock_ctx *) uisize(error)
#define MSG_TYPE_RECV        0x07    //itype   pmsg(struct sock_ctx *)  uisize
#define MSG_TYPE_RECVFROM    0x08    //itype   pmsg(struct udp_recv_msg *) uisize
#define MSG_TYPE_SEND        0x09    //itype   pmsg(struct sock_ctx *)  uisize
#define MSG_TYPE_CLOSE       0x0A    //itype   pmsg(struct sock_ctx *)
#define MSG_TYPE_REQUEST     0x0B    //itype   srcid uisess  pmsg uisize
#define MSG_TYPE_RESPONSE    0x0C    //itype   uisess  pmsg uisize

struct task_ctx;
typedef void *(*module_new)(struct task_ctx *ptask, void *pud);
typedef int32_t(*module_init)(struct task_ctx *ptask, void *pud);
typedef void(*module_run)(struct task_ctx *ptask, uint32_t itype, uint64_t srcid, uint32_t uisess, void *pmsg, uint32_t uisize, void *pud);
typedef void(*module_stop)(struct task_ctx *ptask, void *pud);
typedef void(*module_free)(struct task_ctx *ptask, void *pud);
typedef void(*module_msg_release)(void *pmsg);//任务间消息释放
struct module_ctx
{
    uint32_t maxcnt;//每次最多执行命令数
    module_new md_new;//创建对象
    module_init md_init;//初始化
    module_run md_run;//消息处理
    module_stop md_stop;//停止
    module_free md_free;//释放对象
    char name[NAME_LENS];//任务名
};
struct udp_recv_msg
{
    uint16_t port;
    struct sock_ctx *sock;
    char ip[IP_LENS];
};
/*
* \brief                 初始化
* \param msgfree         消息释放函数
* \param uifreetimeout   任务释放超时时间
* \return                struct srey_ctx
*/
struct srey_ctx *srey_new(uint32_t uiworker, module_msg_release msgfree, uint32_t uifreetimeout);
/*
* \brief           释放
*/
void srey_free(struct srey_ctx *pctx);
/*
* \brief           执行
*/
void srey_loop(struct srey_ctx *pctx);
/*
* \brief           新建
* \param pmodule   struct module_ctx
* \param pudata    用户数据
* \return          NULL 失败
* \return          struct task_ctx
*/
struct task_ctx *srey_newtask(struct srey_ctx *pctx, struct module_ctx *pmodule, void *pudata);
/*
* \brief           获取
* \param pname/id  任务名/ID
* \return          NULL 无
* \return          struct task_ctx
*/
struct task_ctx *srey_grabnam(struct srey_ctx *pctx, const char *pname);
struct task_ctx *srey_grabid(struct srey_ctx *pctx, uint64_t id);
/*
* \brief           释放 srey_newtask/srey_grabnam/srey_grabid
* \param ptask     struct task_ctx
*/
void srey_release(struct task_ctx *ptask);
/*
* \brief           向任务发送消息,无返回数据
* \param ptask     任务
* \param pmsg      消息
* \param uisz      消息长度
*/
void srey_call(struct task_ctx *ptask, void *pmsg, uint32_t uisz);
/*
* \brief           向任务发送消息
* \param ptask     任务
* \param srcid     发起者ID
* \param uisess    标识
* \param pmsg      消息
* \param uisz      消息长度
*/
void srey_request(struct task_ctx *ptask, uint64_t srcid, uint32_t uisess, void *pmsg, uint32_t uisz);
/*
* \brief           返回srey_request发起的请求
* \param ptask     任务
* \param uisess    标识
* \param pmsg      消息
* \param uisz      消息长度
*/
void srey_response(struct task_ctx *ptask, uint32_t uisess, void *pmsg, uint32_t uisz);
/*
* \brief           超时
* \param ptask     任务
* \param uisess    标识
* \param uitimeout 超时时间(毫秒)
*/
void srey_timeout(struct task_ctx *ptask, uint32_t uisess, uint32_t uitimeout);
/*
* \brief           监听
* \param ptask     任务
* \param phost     ip
* \param usport    port
* \return          NULL 失败
* \return          struct listener_ctx
*/
struct listener_ctx *srey_listener(struct task_ctx *ptask, const char *phost, uint16_t usport);
/*
* \brief           监听释放
* \param plsn      struct listener_ctx
*/
void srey_freelsn(struct listener_ctx *plsn);
/*
* \brief           链接
* \param ptask     任务
* \param uisess    标识
* \param utimeout  超时时间(毫秒)
* \param phost     ip
* \param usport    port
* \return          NULL 失败
* \return          struct sock_ctx
*/
struct sock_ctx *srey_connecter(struct task_ctx *ptask, uint32_t uisess, uint32_t utimeout, const char *phost, uint16_t usport);
/*
* \brief           新建struct sock_ctx
* \param ptask     struct srey_ctx
* \param sock      socket
* \param itype     SOCK_STREAM  SOCK_DGRAM
* \param ifamily   AF_INET  AF_INET6
* \return          NULL 失败
* \return          struct sock_ctx
*/
struct sock_ctx *srey_newsock(struct srey_ctx *ptask, SOCKET sock, int32_t itype, int32_t ifamily);
/*
* \brief           开始读写
* \param ptask     任务
* \param psock     struct sock_ctx
* \param iwrite    是否需要写事件, 0否
* \return          ERR_OK 成功
* \return          其他 失败
*/
int32_t srey_enable(struct task_ctx *ptask, struct sock_ctx *psock, int32_t iwrite);
/*
* \brief           获取一session
* \return          session
*/
uint32_t task_new_session(struct task_ctx *ptask);
/*
* \brief           任务ID
* \return          ID
*/
uint64_t task_id(struct task_ctx *ptask);
/*
* \brief           任务名
* \return          任务名
*/
const char *task_name(struct task_ctx *ptask);
/*
* \brief           总执行耗时
* \return          毫秒
*/
uint64_t task_cpucost(struct task_ctx *ptask);
void *task_handle(struct task_ctx *ptask);

extern struct srey_ctx *g_srey;

#endif//SREY_H_
