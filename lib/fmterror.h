#ifndef FMTERROR_H_
#define FMTERROR_H_

#include "os.h"

#ifdef OS_WIN
std::string _fmterror(DWORD error);
#endif

#endif//FMTERROR_H_
