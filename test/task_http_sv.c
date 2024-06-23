#include "task_http_sv.h"

static int32_t _prt = 1;

static void _net_recv(task_ctx *task, SOCKET fd, uint64_t skid, uint8_t pktype, uint8_t client, uint8_t slice, void *data, size_t size) {
    int32_t chunck = http_chunked(data);
    switch (chunck) {
    case 0: {
        binary_ctx bwriter;
        binary_init(&bwriter, NULL, 0, 0);
        http_pack_resp(&bwriter, 200);
        http_pack_head(&bwriter, "Service", "Srey");
        char time[TIME_LENS];
        sectostr(nowsec(), "%Y-%m-%d %H:%M:%S", time);
        http_pack_content(&bwriter, time, strlen(time));
        ev_send(&task->scheduler->netev, fd, skid, bwriter.data, bwriter.offset, 0);
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
