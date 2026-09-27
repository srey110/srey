#include "protocol/mqtt/mqtt_struct.h"

// 释放属性数组及每个属性条目（sval 与条目同一块分配，见 _mqtt_data_kv）
void _mqtt_propertie_free(mprop_arr *properties) {
    if (NULL == properties) {
        return;
    }
    mqtt_propertie *propt;
    for (uint32_t i = 0; i < mprop_arr_size(properties); i++) {
        propt = *mprop_arr_at(properties, (int32_t)i);
        FREE(propt);
    }
    mprop_arr_free(properties);
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
    SECURE_FREE(pl->password, pl->pslens + 1);
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
// PUBLISH 的 varhead / topic / 载荷不单独分配（布局见 mqtt.c 的 _mqtt_publish_blk），
// 也就没有对应的 free：_mqtt_pkfree 只需释放 v5 属性数组
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
    for (uint32_t i = 0; i < msubop_arr_size(&pl->subop); i++) {
        subop = *msubop_arr_at(&pl->subop, (int32_t)i);
        FREE(subop->topic);
        FREE(subop);
    }
    msubop_arr_free(&pl->subop);
    FREE(pl);
}
void _mqtt_unsubscribe_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    void *topic;
    mqtt_unsubscribe_payload *pl = (mqtt_unsubscribe_payload *)data;
    for (uint32_t i = 0; i < mtopic_arr_size(&pl->topics); i++) {
        topic = *mtopic_arr_at(&pl->topics, (int32_t)i);
        FREE(topic);
    }
    mtopic_arr_free(&pl->topics);
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
