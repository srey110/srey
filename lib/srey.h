#ifndef SREY_H_
#define SREY_H_

#include "netev/netev.h"

//struct task_ctx
//{
//    id_t id;
//    uint32_t session;
//    void *instance;
//    void *udata;
//    void *(*task_new)(void);
//    void (*task_free)(void *);
//    srey_task_cb task_cb;
//    char name[NAME_LENS];
//};
//typedef void(*srey_task_cb)(struct task_ctx *ptask, int32_t itype, uint32_t uisource, int32_t isess, void *pmsg, size_t uisize, void *pudata);

struct srey_ctx *srey_new();
void srey_free(struct srey_ctx *pctx);
void srey_loop(struct srey_ctx *pctx);
void srey_wakeup(struct srey_ctx *pctx);
sid_t srey_id(void *pparam);
//struct task_ctx *srey_register(struct srey_ctx *pctx, const char *pname);
//void srey_unregister(struct srey_ctx *pctx, struct task_ctx *ptask);


struct netev_ctx *srey_netev(struct srey_ctx *pctx);
struct tw_ctx *srey_tw(struct srey_ctx *pctx);


#endif//SREY_H_
