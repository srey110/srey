#include "protocol/mqtt/mqtt_struct.h"

void _mqtt_propertie_free(arr_propertie_ctx *properties) {
    if (NULL == properties) {
        return;
    }
    mqtt_propertie *propt;
    for (uint32_t i = 0; i < arr_propertie_size(properties); i++) {
        propt = *arr_propertie_at(properties, i);
        FREE(propt->sval);
        FREE(propt);
    }
    arr_propertie_free(properties);
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
void _mqtt_puback_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_puback_varhead *vh = (mqtt_puback_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_pubrec_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_pubrec_varhead *vh = (mqtt_pubrec_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_pubrel_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_pubrel_varhead *vh = (mqtt_pubrel_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_pubcomp_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_pubcomp_varhead *vh = (mqtt_pubcomp_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_subscribe_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_subscribe_varhead *vh = (mqtt_subscribe_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_subscribe_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    subscribe_option *subop;
    mqtt_subscribe_payload *pl = (mqtt_subscribe_payload *)data;
    for (uint32_t i = 0; i < arr_subscribe_option_size(&pl->subop); i++) {
        subop = *arr_subscribe_option_at(&pl->subop, i);
        FREE(subop->topic);
        FREE(subop);
    }
    arr_subscribe_option_free(&pl->subop);
    FREE(pl);
}
void _mqtt_suback_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_suback_varhead *vh = (mqtt_suback_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_suback_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_suback_payload *pl = (mqtt_suback_payload *)data;
    FREE(pl);
}
void _mqtt_unsubscribe_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_unsubscribe_varhead *vh = (mqtt_unsubscribe_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_unsubscribe_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    void *topic;
    mqtt_unsubscribe_payload *pl = (mqtt_unsubscribe_payload *)data;
    for (uint32_t i = 0; i < arr_ptr_size(&pl->topics); i++) {
        topic = *arr_ptr_at(&pl->topics, i);
        FREE(topic);
    }
    arr_ptr_free(&pl->topics);
    FREE(pl);
}
void _mqtt_unsuback_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_unsuback_varhead *vh = (mqtt_unsuback_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_unsuback_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_unsuback_payload *pl = (mqtt_unsuback_payload *)data;
    FREE(pl);
}
void _mqtt_disconnect_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_disconnect_varhead *vh = (mqtt_disconnect_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
void _mqtt_auth_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_auth_varhead *vh = (mqtt_auth_varhead *)data;
    _mqtt_propertie_free(vh->properties);
    FREE(vh);
}
