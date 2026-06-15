#include "containers/fsqu.h"

#define FSQU_DEFAULT_CAP  1024

void fsqu_init(fsqu_ctx *fsqu, size_t elsize, uint32_t capacity) {
    capacity = (0 == capacity ? FSQU_DEFAULT_CAP : capacity);
#if FSQU_MPQ
    mpq_init(&fsqu->qu, elsize, capacity);
#else
    queue_init(&fsqu->qu, elsize, capacity);
    spin_init(&fsqu->lck, SPIN_CNT);
#endif
}
void fsqu_free(fsqu_ctx *fsqu) {
#if FSQU_MPQ
    mpq_free(&fsqu->qu);
#else
    queue_free(&fsqu->qu);
    spin_free(&fsqu->lck);
#endif
}
