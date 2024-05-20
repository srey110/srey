#ifndef SARRAY_H_
#define SARRAY_H_

#include "structs.h"

#define ARRAY_INIT_SIZE      16

#define ARRAY_DECL(type, atype) \
typedef struct atype {      \
    type   *ptr;    \
    uint32_t  size;   \
    uint32_t  maxsize;\
}atype##_ctx;\
static inline void atype##_init(atype##_ctx *p, uint32_t maxsize) {\
    p->size = 0;\
    p->maxsize = ((0 == maxsize) ? ARRAY_INIT_SIZE : ROUND_UP(maxsize, 2));\
    MALLOC(p->ptr, sizeof(type) * p->maxsize);\
};\
static inline void atype##_clear(atype##_ctx *p) {\
    p->size = 0;\
};\
static inline void atype##_free(atype##_ctx *p) {\
    FREE(p->ptr);\
};\
static inline uint32_t atype##_size(atype##_ctx *p) {\
    return p->size;\
};\
static inline uint32_t atype##_maxsize(atype##_ctx *p) {\
    return p->maxsize;\
};\
static inline int32_t atype##_empty(atype##_ctx *p) {\
    return 0 == p->size;\
};\
static inline void atype##_resize(atype##_ctx *p, uint32_t maxsize) {\
    maxsize = ((0 == maxsize) ? ARRAY_INIT_SIZE : ROUND_UP(maxsize, 2));\
    ASSERTAB(maxsize >= p->size, "max size must big than element count.");\
    REALLOC(p->ptr, p->ptr, sizeof(type) * maxsize);\
    p->maxsize = maxsize;\
};\
static inline void atype##_double_resize(atype##_ctx *p) {\
    atype##_resize(p, p->maxsize * 2);\
};\
static inline type *atype##_at(atype##_ctx *p, uint32_t pos) {\
    if (pos < 0){\
        pos += p->size;\
    }\
    ASSERTAB(pos >= 0 && pos < p->size, "array pos error.");\
    return p->ptr + pos;\
};\
static inline type *atype##_front(atype##_ctx *p) {\
    return 0 == p->size ? NULL : p->ptr;\
};\
static inline type *atype##_back(atype##_ctx *p) {\
    return 0 == p->size ? NULL : p->ptr + p->size - 1;\
};\
static inline void atype##_push_back(atype##_ctx *p, type *elem) {\
    if (p->size == p->maxsize) {\
        atype##_double_resize(p);\
    }\
    p->ptr[p->size] = *elem;\
    p->size++;\
};\
static inline type *atype##_pop_back(atype##_ctx *p) {\
    if(0 == p->size) return NULL;\
    type *ptr = p->ptr + p->size - 1; \
    p->size--; \
    return ptr;\
};\
static inline void atype##_add(atype##_ctx *p, type *elem, uint32_t pos) {\
    if (pos < 0) {\
        pos += p->size;\
    }\
    ASSERTAB(pos >= 0 && pos <= p->size, "pos error.");\
    if (p->size == p->maxsize) {\
        atype##_double_resize(p);\
    }\
    if (pos < p->size) {\
        memmove(p->ptr + pos+1, p->ptr + pos, sizeof(type) * (p->size - pos));\
    }\
    p->ptr[pos] = *elem;\
    p->size++;\
};\
static inline void atype##_del(atype##_ctx *p, uint32_t pos) {\
    if (pos < 0) {\
        pos += p->size;\
    }\
    ASSERTAB(pos >= 0 && pos < p->size, "pos error.");\
    p->size--;\
    if (pos < p->size) {\
        memmove(p->ptr + pos, p->ptr + pos+1, sizeof(type) * (p->size - pos));\
    }\
};\
static inline void atype##_del_nomove(atype##_ctx *p, uint32_t pos) {\
    if (pos < 0) {\
        pos += p->size;\
    }\
    ASSERTAB(pos >= 0 && pos < p->size, "pos error.");\
    p->size--;\
    if (pos < p->size) {\
        p->ptr[pos] = p->ptr[p->size];\
    }\
};\
static inline void atype##_swap(atype##_ctx *p, uint32_t pos1, uint32_t pos2) {\
    if (pos1 < 0) {\
        pos1 += p->size;\
    }\
    if (pos2 < 0) {\
        pos2 += p->size;\
    }\
    type tmp = p->ptr[pos1];\
    p->ptr[pos1] = p->ptr[pos2];\
    p->ptr[pos2] = tmp;\
};\

ARRAY_DECL(void *, arr_ptr);

#endif//SARRAY_H_
