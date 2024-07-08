#ifndef LOADER_H_
#define LOADER_H_

#include "srey/spub.h"

loader_ctx *loader_init(uint16_t nnet, uint16_t nworker);
void loader_free(loader_ctx *loader);

#endif//LOADER_H_
