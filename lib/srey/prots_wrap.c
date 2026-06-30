#include "srey/prots_wrap.h"

int32_t mysql_try_connect(task_ctx *task, mysql_ctx *mysql) {
    mysql->task = task;
    return task_connect(task, PACK_MYSQL, NULL, mysql->client.ip, mysql->client.port,
        NULL == mysql->client.evssl ? NETEV_NONE : NETEV_AUTHSSL, mysql, &mysql->client.sk.fd, &mysql->client.sk.skid);
}
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg) {
    pg->task = task;
    return task_connect(task, PACK_PGSQL, NULL, pg->ip, pg->port, NETEV_AUTHSSL, pg, &pg->sk.fd, &pg->sk.skid);
}
int32_t mongo_try_connect(task_ctx *task, mongo_ctx *mongo) {
    mongo->task = task;
    return task_connect(task, PACK_MONGO, NULL, mongo->ip, mongo->port,
        NULL == mongo->evssl ? NETEV_NONE : NETEV_AUTHSSL, mongo, &mongo->sk.fd, &mongo->sk.skid);
}
int32_t smtp_try_connect(task_ctx *task, smtp_ctx *smtp) {
    smtp->task = task;
    return task_connect(task, PACK_SMTP, smtp->evssl, smtp->ip, smtp->port, 0, smtp, &smtp->sk.fd, &smtp->sk.skid);
}
int32_t mqtt_try_connect(task_ctx *task, struct evssl_ctx *evssl,
                         const char *ip, uint16_t port, int32_t netev,
                         mqtt_protversion version, SOCKET *fd, uint64_t *skid) {
    mqtt_ctx *mq;
    MALLOC(mq, sizeof(mqtt_ctx));
    mq->version = version;
    return task_connect(task, PACK_MQTT, evssl, ip, port, netev, mq, fd, skid);
}
