#ifndef FUNCFORMACRO_H_
#define FUNCFORMACRO_H_

#include "os.h"

#ifdef OS_WIN
std::string _fmterror(DWORD error);
#endif

#ifdef ATOMIC_GUN
uint32_t _fetchandset(volatile uint32_t *ptr, uint32_t value);
#endif

#endif//FUNCFORMACRO_H_
