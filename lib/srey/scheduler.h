#ifndef SCHEDULER_H_
#define SCHEDULER_H_

#include "srey/spub.h"

scheduler_ctx *scheduler_init(uint16_t nnet, uint16_t nworker);
void scheduler_free(scheduler_ctx *scheduler);

#endif//SCHEDULER_H_
