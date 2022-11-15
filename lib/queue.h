#ifndef QUEUE_H_
#define QUEUE_H_

#include "macro.h"

#define QUEUE_INIT_SIZE 16

#define QUEUE_DECL(type, qtype) \
struct qtype { \
    size_t offset; \
    size_t size; \
    size_t maxsize; \
    type   *ptr; \
}; \
typedef struct qtype qtype;\
static inline void qtype##_init(qtype *p, size_t maxsize) {\
    p->size = p->offset = 0;\
    p->maxsize = ((0 == maxsize) ? QUEUE_INIT_SIZE : ROUND_UP(maxsize, 2));\
    MALLOC(p->ptr, sizeof(type) * p->maxsize);\
};\
static inline void qtype##_clear(qtype *p) {\
    p->size = p->offset = 0;\
};\
static inline void qtype##_free(qtype *p) {\
    FREE(p->ptr);\
};\
static inline size_t qtype##_size(qtype *p) {\
    return p->size;\
};\
static inline size_t qtype##_maxsize(qtype *p) {\
    return p->maxsize;\
};\
static inline int32_t qtype##_empty(qtype* p) {\
    return 0 == p->size;\
};\
static inline void qtype##_resize(qtype *p, size_t maxsize) {\
    maxsize = ((0 == maxsize) ? QUEUE_INIT_SIZE : ROUND_UP(maxsize, 2));\
    ASSERTAB(maxsize >= p->size, "max size must big than element count.");\
    type  *pnew;\
    MALLOC(pnew, sizeof(type) * maxsize);\
    for (size_t i = 0; i < p->size; i++){\
        pnew[i] = p->ptr[(p->offset + i) % p->maxsize];\
    }\
    FREE(p->ptr);\
    p->offset = 0;\
    p->ptr = pnew;\
    p->maxsize = maxsize;\
};\
static inline void qtype##_push(qtype *p, type *elem) {\
    if(p->size == p->maxsize){\
        qtype##_resize(p, p->maxsize * 2);\
    }\
    size_t pos = p->offset + p->size;\
    if (pos >= p->maxsize) {\
        pos -= p->maxsize;\
    }\
    p->ptr[pos] = *elem;\
    p->size++;\
};\
static inline type *qtype##_pop(qtype *p) {\
    if (0 == p->size) return NULL;\
    type *elem = p->ptr + p->offset;\
    p->offset++;\
    p->size--;\
    if (p->offset >= p->maxsize) {\
        p->offset -= p->maxsize;\
    }\
    return elem;\
};\
static inline type *qtype##_peek(qtype *p) {\
    if (0 == p->size) return NULL;\
    return p->ptr + p->offset;\
};\

QUEUE_DECL(void *, qu_void);

#endif//QUEUE_H_
