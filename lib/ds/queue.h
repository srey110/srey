#ifndef QUEUE_H_
#define QUEUE_H_

#include "macro.h"

#define QUEUE_INIT_SIZE      16

#define QUEUE_DECL(type, qtype) \
typedef struct qtype { \
    uint32_t offset; \
    uint32_t size; \
    uint32_t maxsize; \
    type   *ptr; \
}qtype##_ctx; \
static inline void qtype##_init(qtype##_ctx *p, uint32_t maxsize) {\
    p->size = p->offset = 0;\
    p->maxsize = ((0 == maxsize) ? QUEUE_INIT_SIZE : ROUND_UP(maxsize, 2));\
    MALLOC(p->ptr, sizeof(type) * p->maxsize);\
};\
static inline void qtype##_clear(qtype##_ctx *p) {\
    p->size = p->offset = 0;\
};\
static inline void qtype##_free(qtype##_ctx *p) {\
    FREE(p->ptr);\
};\
static inline uint32_t qtype##_size(qtype##_ctx *p) {\
    return p->size;\
};\
static inline uint32_t qtype##_maxsize(qtype##_ctx *p) {\
    return p->maxsize;\
};\
static inline int32_t qtype##_empty(qtype##_ctx *p) {\
    return 0 == p->size;\
};\
static inline void qtype##_resize(qtype##_ctx *p, uint32_t maxsize) {\
    maxsize = ((0 == maxsize) ? QUEUE_INIT_SIZE : ROUND_UP(maxsize, 2));\
    ASSERTAB(maxsize >= p->size, "max size must big than element count.");\
    type  *pnew;\
    MALLOC(pnew, sizeof(type) * maxsize);\
    for (uint32_t i = 0; i < p->size; i++){\
        pnew[i] = p->ptr[(p->offset + i) % p->maxsize];\
    }\
    FREE(p->ptr);\
    p->offset = 0;\
    p->ptr = pnew;\
    p->maxsize = maxsize;\
};\
static inline void qtype##_push(qtype##_ctx *p, type *elem) {\
    if(p->size == p->maxsize){\
        qtype##_resize(p, p->maxsize * 2);\
    }\
    uint32_t pos = p->offset + p->size;\
    if (pos >= p->maxsize) {\
        pos -= p->maxsize;\
    }\
    p->ptr[pos] = *elem;\
    p->size++;\
};\
static inline type *qtype##_pop(qtype##_ctx *p) {\
    if (0 == p->size) return NULL;\
    type *elem = p->ptr + p->offset;\
    p->offset++;\
    p->size--;\
    if (p->offset >= p->maxsize) {\
        p->offset -= p->maxsize;\
    }\
    return elem;\
};\
static inline type *qtype##_peek(qtype##_ctx *p) {\
    if (0 == p->size) return NULL;\
    return p->ptr + p->offset;\
};\
static inline type *qtype##_at(qtype##_ctx *p, uint32_t pos) {\
    if (0 == p->size || pos >= p->size) return NULL;\
    uint32_t curpos = p->offset + pos;\
    if (curpos >= p->maxsize) {\
        return p->ptr + (curpos - p->maxsize);\
    } else {\
        return p->ptr + (p->offset + pos);\
    }\
};\

QUEUE_DECL(void *, qu_ptr);

#endif//QUEUE_H_
