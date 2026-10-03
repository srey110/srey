#include "protocol/mqtt/mqtt_struct.h"

// 布局见 mqtt_prop_blk：块内小区里的属性随块走，小区外另开的逐个放，指针数组搬到堆上的也放
void _mqtt_prop_blk_release(mprop_arr *properties) {
    mqtt_prop_blk *blk = (mqtt_prop_blk *)properties;
    mqtt_propertie *propt;
    for (uint32_t i = 0; i < mprop_arr_size(properties); i++) {
        propt = *mprop_arr_at(properties, (int32_t)i);
        if ((char *)propt < (char *)blk->arena
            || (char *)propt >= (char *)blk->arena + blk->cap) {
            FREE(propt);
        }
    }
    if (properties->ptr != blk->slots) {
        mprop_arr_free(properties);
    }
}
void _mqtt_prop_blk_free(mprop_arr *properties) {
    _mqtt_prop_blk_release(properties);
    FREE(properties);
}
void _mqtt_connect_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_connect_varhead *vh = (mqtt_connect_varhead *)data;
    _mqtt_propertie_free(vh->properties);
}
// 各串与载荷同一块、密码排在最后(见 mqtt.c 的 _mqtt_connect_str)，擦到密码的结尾 NUL 即盖住块里写过的全部
void _mqtt_connect_payload_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_connect_payload * pl = (mqtt_connect_payload *)data;
    _mqtt_propertie_free(pl->properties);
    if (NULL == pl->password) {
        FREE(pl);
        return;
    }
    SECURE_FREE(pl, (size_t)(pl->password + pl->pslens + 1 - (char *)pl));
}
void _mqtt_connack_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_connack_varhead *vh = (mqtt_connack_varhead *)data;
    _mqtt_propertie_free(vh->properties);
}
// PUBLISH 的 varhead / topic / 载荷不单独分配（布局见 mqtt.c 的 _mqtt_publish_blk），
// 也就没有对应的 free：_mqtt_pkfree 只需释放 v5 属性数组
void _mqtt_pubackrel_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_pubackrel_varhead *vh = (mqtt_pubackrel_varhead *)data;
    _mqtt_propertie_free(vh->properties);
}
void _mqtt_subreqresp_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_subreqresp_varhead *vh = (mqtt_subreqresp_varhead *)data;
    _mqtt_propertie_free(vh->properties);
}
void _mqtt_reason_varhead_free(void *data) {
    if (NULL == data) {
        return;
    }
    mqtt_reason_varhead *vh = (mqtt_reason_varhead *)data;
    _mqtt_propertie_free(vh->properties);
}
