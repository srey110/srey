#ifndef ROUTER_PUB_H_
#define ROUTER_PUB_H_

#include "base/base.h"

// 路由 id 由 router 的路由表分配；router_tree 只保存，不释放。
typedef uint32_t router_route_id;
// 路径参数 / 可选段上限。
// 这两个值同时约束注册期解析和 router_tree 的变体展开，必须只有这一份。
#define ROUTER_MAX_PARAMS 16
#define ROUTER_MAX_OPT     8
#define ROUTER_ROUTE_NONE UINT32_MAX // 路由 id 空值

// HTTP 方法位掩码；单个路由可通过按位或组合（ROUTER_M_ANY 匹配所有已知方法）
typedef enum router_method {
    ROUTER_M_GET     = 1 << 0, // GET
    ROUTER_M_POST    = 1 << 1, // POST
    ROUTER_M_PUT     = 1 << 2, // PUT
    ROUTER_M_DELETE  = 1 << 3, // DELETE
    ROUTER_M_PATCH   = 1 << 4, // PATCH
    ROUTER_M_HEAD    = 1 << 5, // HEAD
    ROUTER_M_OPTIONS = 1 << 6, // OPTIONS
    ROUTER_M_ANY     = 0xFF    // 所有已知方法
} router_method;
// 路径段类型；router_seg_index 交回，反向生成 URL 的调用方据此还原模板
typedef enum router_seg_type {
    ROUTER_SEG_LIT,    // 字面量
    ROUTER_SEG_PARAM,  // {name}    必填路径参数
    ROUTER_SEG_OPT,    // {name?}   可选路径参数
    ROUTER_SEG_WILD    // *         末尾通配，出现即吞掉后续所有请求段
} router_seg_type;

// 一条已解析的路径段。
// str 归 router 的路由记录所有；router_tree 只借用，不释放。
// WILD 段 str 为 NULL，str_len 为 0。
typedef struct router_seg {
    uint32_t str_len;       // 段字节数，不含结尾 '\0'
    router_seg_type t;      // 段类型
    char *str;              // LIT/PARAM/OPT 的文本；WILD 为 NULL
} router_seg;

#endif // ROUTER_PUB_H_
