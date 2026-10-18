#include "srey/prots_wrap.h"
#include "protocol/websock.h"

// 同一 ctx 上的旧连接还没关完(sk 仍有效)就不发起：新连接会覆写 sk，旧连接晚到的关闭回调
// 又会把新 sk 置成失效、清掉它的解析状态。旧连接须先经 coro_close 确认关完
static inline int32_t _try_connect_check(sock_ctx *sk, const char *name) {
    if (sock_is_invalid(sk)) {
        return ERR_OK;
    }
    LOG_WARN("%s: previous connection not closed yet, connect refused.", name);
    return ERR_FAILED;
}
int32_t mysql_try_connect(task_ctx *task, mysql_ctx *mysql, int32_t setsess) {
    if (ERR_OK != _try_connect_check(&mysql->client.sk, "mysql")) {
        return ERR_FAILED;
    }
    mysql->task = task;
    PROT_REF_ACQUIRE(mysql);// connect失败由prots_udfree兜底减引用
    return task_connect(task, PACK_MYSQL, NULL, mysql->client.ip, mysql->client.port,
        NULL == mysql->client.evssl ? NETEV_NONE : NETEV_AUTHSSL,
        mysql, setsess, &mysql->client.sk);
}
int32_t pgsql_try_connect(task_ctx *task, pgsql_ctx *pg, int32_t setsess) {
    if (ERR_OK != _try_connect_check(&pg->sk, "pgsql")) {
        return ERR_FAILED;
    }
    pg->task = task;
    PROT_REF_ACQUIRE(pg);
    return task_connect(task, PACK_PGSQL, NULL, pg->ip, pg->port,
        NETEV_AUTHSSL, pg, setsess, &pg->sk);
}
int32_t mongo_try_connect(task_ctx *task, mongo_ctx *mongo, int32_t setsess) {
    if (ERR_OK != _try_connect_check(&mongo->sk, "mongo")) {
        return ERR_FAILED;
    }
    mongo->task = task;
    PROT_REF_ACQUIRE(mongo);
    return task_connect(task, PACK_MONGO, mongo->evssl, mongo->ip, mongo->port,
        NETEV_NONE, mongo, setsess, &mongo->sk);
}
int32_t smtp_try_connect(task_ctx *task, smtp_ctx *smtp, int32_t setsess) {
    if (ERR_OK != _try_connect_check(&smtp->sk, "smtp")) {
        return ERR_FAILED;
    }
    smtp->task = task;
    PROT_REF_ACQUIRE(smtp);
    return task_connect(task, PACK_SMTP, smtp->evssl, smtp->ip, smtp->port,
        NETEV_NONE, smtp, setsess, &smtp->sk);
}
int32_t mqtt_try_connect(task_ctx *task, struct evssl_ctx *evssl,
                         const char *ip, uint16_t port, int32_t netev,
                         mqtt_protversion version, int32_t setsess, sock_ctx *sk) {
    mqtt_ctx *mq = mqtt_ctx_new(version);
    if (NULL == mq) {
        return ERR_FAILED;
    }
    return task_connect(task, PACK_MQTT, evssl, ip, port, netev, mq, setsess, sk);
}
int32_t mqtt_ws_bind(task_ctx *task, sock_ctx *sk, mqtt_protversion version) {
    mqtt_ctx *mq = mqtt_ctx_new(version);
    if (NULL == mq) {
        return ERR_FAILED;
    }
    return websock_set_secextra(&task->loader->netev, sk, mq, mqtt_ctx_free);
}
