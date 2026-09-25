#include "serial/yyjson/yyjson_helper.h"
#include "utils/utils.h"

static void *_yyjson_malloc(void *ctx, size_t size) {
    (void)ctx;
    return _malloc(size);
}
static void *_yyjson_realloc(void *ctx, void *ptr, size_t osize, size_t size) {
    (void)ctx;
    (void)osize;
    return _realloc(ptr, size);
}
static void _yyjson_free(void *ctx, void *ptr) {
    (void)ctx;
    _free(ptr);
}
// yyjson 的默认分配器。yyjson.c 顶部 #define YYJSON_CUSTOM_ALC 指向这里，
// 所有 alc 形参传 NULL 的调用都会落到框架分配器上，计入 MEMORY_CHECK
const yyjson_alc g_yyjson_alc = {
    _yyjson_malloc, _yyjson_realloc, _yyjson_free, NULL
};

int32_t json_has(yyjson_val *json, const char *name) {
    return NULL != yyjson_obj_get(json, name);
}
int32_t json_val_number(yyjson_val *jval, double *val) {
    if (NULL == jval
        || !yyjson_is_num(jval)) {
        return ERR_FAILED;
    }
    double d = yyjson_get_num(jval);
    if (isnan(d) || isinf(d)) {
        return ERR_FAILED;
    }
    *val = d;
    return ERR_OK;
}
int32_t json_get_number(yyjson_val *json, const char *name, double *val) {
    return json_val_number(yyjson_obj_get(json, name), val);
}
int32_t json_val_num_range(yyjson_val *jval, double min, double max, double *val) {
    double num = 0;
    if (ERR_OK != json_val_number(jval, &num)) {
        return ERR_FAILED;
    }
    if (num < min
        || num > max) {
        return ERR_FAILED;
    }
    *val = num;
    return ERR_OK;
}
int32_t json_get_num_range(yyjson_val *json, const char *name, double min, double max, double *val) {
    return json_val_num_range(yyjson_obj_get(json, name), min, max, val);
}
int32_t json_val_string(yyjson_val *val, char *str, size_t lens) {
    if (NULL == val
        || !yyjson_is_str(val)) {
        return ERR_FAILED;
    }
    const char *sval = yyjson_get_str(val);
    size_t slens = yyjson_get_len(val);
    // 目标是定长 C 串缓冲，含内嵌 NUL 的值取用时必在那里断掉,收下就是静默改值
    if (NULL != memchr(sval, '\0', slens)) {
        return ERR_FAILED;
    }
    // 用 yyjson 记的真实长度而不是 strlen：`"sc\u0000ript"` 是合法 JSON，strlen 只看到 2，
    // 装不下的值会被误判成装得下
    return copy_bounded(sval, slens, str, lens, 1);
}
int32_t json_get_string(yyjson_val *json, const char *name, char *str, size_t lens) {
    return json_val_string(yyjson_obj_get(json, name), str, lens);
}
