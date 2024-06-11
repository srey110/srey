#ifndef LOG_H_
#define LOG_H_

#include "base/macro.h"

void log_init(FILE *file);
void log_setlv(LOG_LEVEL lv);

#endif//LOG_H_
