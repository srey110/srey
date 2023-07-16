#include "test2.h"

void test2_run(task_ctx *task, message_ctx *msg) {
    switch (msg->mtype) {
    case MSG_TYPE_STARTUP: {
        struct evssl_ctx *ssl = srey_ssl_qury(task->srey, SSL_SERVER);
        if (NULL == ssl) {
            LOG_WARN("srey_call error.");
        } else {
            uint64_t lsnid;
            if (ERR_OK != srey_listen(task, PACK_SIMPLE, ssl, "0.0.0.0", 15001, 0, &lsnid)) {
                LOG_WARN("srey_listen error.");
            }
        }
        break;
    }
    case MSG_TYPE_ACCEPT:
        //LOG_INFO("accept socket %d.", (uint32_t)msg->fd);
        break;
    case MSG_TYPE_RECV: {
#if RAND_CLOSE
        if (randrange(1, 100) <= 1) {
            ev_close(&task->srey->netev, msg->fd, msg->skid);
            break;
        }
#endif
        size_t lens;
        void *data = simple_data(msg->data, &lens);
        void *pack = simple_pack(data, lens, &lens);
        ev_send(&task->srey->netev, msg->fd, msg->skid, pack, lens, 0);
        break;
    }
    case MSG_TYPE_SEND:
        //LOG_INFO("socket %d send %d byte.", (uint32_t)msg->fd, (uint32_t)msg->size);
        break;
    case MSG_TYPE_CLOSE:
        //LOG_INFO("socket %d closed.", (uint32_t)msg->fd);
        break;
    default:
        break;
    }
}
