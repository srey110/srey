#include "task_http_sv.h"

static int32_t _prt = 1;

static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    int32_t chunck = http_chunked(data);
    switch (chunck) {
    case 0: {
        buffer_ctx buf;
        buffer_init(&buf);
        http_pack_resp(&buf, 200);
        http_pack_head(&buf, "Service", "Srey");
        char time[TIME_LENS];
        sectostr(nowsec(), "%Y-%m-%d %H:%M:%S", time);
        http_pack_content(&buf, time, strlen(time));
        char *httpdata;
        size_t lens = buffer_size(&buf);
        MALLOC(httpdata, lens);
        buffer_copyout(&buf, 0, httpdata, lens);
        ev_send(&task->scheduler->netev, fd, skid, httpdata, lens, 0);
        buffer_free(&buf);
        break;
    }
    case 1: {
        break;
    }
    case 2: {
        break;
    }
    default:
        break;
    }
}
static void _startup(task_ctx *task) {
    on_recved(task, _net_recv);
    uint64_t id;
    trigger_listen(task, PACK_HTTP, NULL, "0.0.0.0", 15003, &id, 0);
}
void task_http_sv_start(scheduler_ctx *scheduler, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(scheduler, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
