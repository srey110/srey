#include "protocol/mqtt/mqtt_struct.h"

// 释放属性数组及每个属性条目（含 sval）
void _mqtt_propertie_free(array_ctx *properties) {
    if (NULL == properties) {
        return;
    }
    mqtt_propertie *propt;
    for (uint32_t i = 0; i < array_size(properties); i++) {
        propt = *(mqtt_propertie **)array_at(properties, i);
        FREE(propt->sval);
        FREE(propt);
    }
    array_free(properties);
    FREE(properties);
}
void _mqtt_connect_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_connect_varhead *vh = (mqtt_connect_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_connect_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_connect_payload * pl = (mqtt_connect_payload *)data;
    FREE(pl->clientid);
    _mqtt_propertie_free(pl->properties);
    FREE(pl->willtopic);
    FREE(pl->willpayload);
    FREE(pl->user);
    FREE(pl->password);
    FREE(pl);
}
void _mqtt_connack_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_connack_varhead *vh = (mqtt_connack_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_publish_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_publish_varhead *vh = (mqtt_publish_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh->topic);
    FREE(vh);
}
void _mqtt_publish_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_publish_payload *pl = (mqtt_publish_payload *)data;
    FREE(pl);
}
void _mqtt_pubackrel_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_pubackrel_varhead *vh = (mqtt_pubackrel_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_subreqresp_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_subreqresp_varhead *vh = (mqtt_subreqresp_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_subscribe_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    subscribe_option *subop;
    mqtt_subscribe_payload *pl = (mqtt_subscribe_payload *)data;
    for (uint32_t i = 0; i < array_size(&pl->subop); i++) {
        subop = *(subscribe_option **)array_at(&pl->subop, i);
        FREE(subop->topic);
        FREE(subop);
    }
    array_free(&pl->subop);
    FREE(pl);
}
void _mqtt_unsubscribe_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    void *topic;
    mqtt_unsubscribe_payload *pl = (mqtt_unsubscribe_payload *)data;
    for (uint32_t i = 0; i < array_size(&pl->topics); i++) {
        topic = *(void **)array_at(&pl->topics, i);
        FREE(topic);
    }
    array_free(&pl->topics);
    FREE(pl);
}
void _mqtt_reasonlist_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_reasonlist_payload *pl = (mqtt_reasonlist_payload *)data;
    FREE(pl);
}
void _mqtt_reason_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
