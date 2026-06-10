#include "containers/fsqu.h"

void fsqu_init(fsqu_ctx *fsqu, size_t elsize, uint32_t capacity) {
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
