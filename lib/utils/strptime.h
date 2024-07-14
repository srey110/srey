#ifndef STRPTIME_H_
#define STRPTIME_H_

#include "base/macro.h"

//字符串格式化为时间
char *_strptime(const char *buf, const char *fmt, struct tm *tm);

#endif//STRPTIME_H_
