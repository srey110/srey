#include "task_mqttsv.h"

static int32_t _prt = 1;

static void _startup(task_ctx *task) {
    uint64_t id;
    task_listen(task, PACK_MQTT, NULL, "0.0.0.0", 15005, &id, 0);
}
void task_mqtt_sv_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    task_ctx *task = task_new(loader, name, NULL, NULL, NULL);
    task_register(task, _startup, NULL);
}
