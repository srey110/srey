#ifndef STRPTIME_H_
#define STRPTIME_H_

#include "base/macro.h"

#ifdef OS_WIN
char* strptime(const char *buf, const char *fmt, struct tm *tm);
#endif
#endif//STRPTIME_H_
