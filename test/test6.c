#include "test6.h"
static FILE *file = NULL;
static void  _http_recv(task_ctx *task, message_ctx *msg) {
    int32_t chunck = http_chunked(msg->data);
    switch (chunck) {
    case 0: {
        buffer_ctx buf;
        buffer_init(&buf);
        http_pack_resp(&buf, 200);
        http_pack_head(&buf, "Server", "Srey");
        char time[TIME_LENS];
        nowtime("%Y-%m-%d %H:%M:%S ", time);
        size_t rlen;
        char *resp = http_pack_content(&buf, time, strlen(time), &rlen);
        ev_send(&task->srey->netev, msg->fd, msg->skid, resp, rlen, 0);
        buffer_free(&buf);
        break;
    }
    case 1: {
        char time[TIME_LENS];
        nowtime("%H-%M-%S ", time);
        char filename[PATH_LENS] = { 0 };
        SNPRINTF(filename, sizeof(filename) - 1, "%s%s%d %s", procpath(), PATH_SEPARATORSTR, (uint32_t)msg->skid, time);
        file = fopen(filename, "wb");
        break;
    }
    case 2: {
        size_t lens;
        char *pack = http_data(msg->data, &lens);
        if (0 == lens) {
            fclose(file);

            char ckdata[128];
            size_t rlen;
            char *resp;
            buffer_ctx buf;
            buffer_init(&buf);
            http_pack_resp(&buf, 200);
            http_pack_head(&buf, "Server", "Srey");
            for (int32_t i = 0; i < 3; i++) {
                ZERO(ckdata, sizeof(ckdata));
                SNPRINTF(ckdata, sizeof(ckdata) - 1, "1234567890 %d.", i);
                resp = http_pack_chunked(&buf, ckdata, strlen(ckdata), &rlen);
                ev_send(&task->srey->netev, msg->fd, msg->skid, resp, rlen, 0);
            }
            resp = http_pack_chunked(&buf, NULL, 0, &rlen);
            ev_send(&task->srey->netev, msg->fd, msg->skid, resp, rlen, 0);
        } else {
            fwrite(pack, 1, lens, file);
        }
        break;
    }
    default:
        break;
    }
}
static void _websock_recv(task_ctx *task, message_ctx *msg) {
    int32_t fin = websock_pack_fin(msg->data);
    int32_t proto = websock_pack_proto(msg->data);
    switch (proto) {
    case WBSK_CLOSE: {
        ev_close(&task->srey->netev, msg->fd, msg->skid);
        break;
    }
    case WBSK_PING: {
        websock_pong(&task->srey->netev, msg->fd, msg->skid, 0);
        break;
    }
    default: {
        size_t lens;
        char cbuf[128];
        char time[TIME_LENS];
        nowtime("%Y-%m-%d %H:%M:%S ", time);
        char *data = websock_pack_data(msg->data, &lens);
        if (0 == memcmp(data, "chunked", strlen("chunked"))) {
            websock_text(&task->srey->netev, msg->fd, msg->skid, 0, 0, time, strlen(time));
            for (int32_t i = 0; i < 3; i++) {
                ZERO(cbuf, sizeof(cbuf));
                SNPRINTF(cbuf, sizeof(cbuf) - 1, "%s %d ", time, i);
                websock_continuation(&task->srey->netev, msg->fd, msg->skid, 0, 0, cbuf, strlen(cbuf));
            }
            websock_continuation(&task->srey->netev, msg->fd, msg->skid, 0, 1, " over.", strlen(" over."));
        } else {
            websock_text(&task->srey->netev, msg->fd, msg->skid, 0, 1, time, strlen(time));
        }
        break;
    }
    }
    
}
void test6_run(task_ctx *task, message_ctx *msg) {
    switch (msg->mtype) {
    case MSG_TYPE_STARTUP: {
        uint64_t lsnid;
        srey_listen(task, PACK_HTTP, NULL, "0.0.0.0", 15004, 0, &lsnid);
        srey_listen(task, PACK_WEBSOCK, NULL, "0.0.0.0", 15003, 0, &lsnid);
        break;
    }
    case MSG_TYPE_RECV: {
        if (PACK_HTTP == msg->pktype) {
            _http_recv(task, msg);
        } else if (PACK_WEBSOCK == msg->pktype) {
            _websock_recv(task, msg);
        }
        break;  
    }
    default:
        break;
    }
}
