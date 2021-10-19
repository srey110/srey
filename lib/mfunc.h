#ifndef MACFUNC_H_
#define MACFUNC_H_

#include "os.h"

#if defined(OS_WIN)
static inline const char *_fmterror(DWORD error)
{
    char *perror = NULL;
    if (0 == FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
        (LPTSTR)&perror,
        0,
        NULL))
    {
        return "FormatMessageA error.";
    }

    static char errstr[512];
    size_t ilens = strlen(perror);
    ilens = ilens >= sizeof(errstr) ? sizeof(errstr) - 1 : ilens;
    memcpy(errstr, perror, ilens);
    errstr[ilens] = '\0';
    LocalFree(perror);

    return errstr;
}
#elif defined(OS_SUN)
#else
static inline uint32_t _fetchandset(volatile uint32_t *ptr, uint32_t value)
{
    uint32_t oldvar;

    do
    {
        oldvar = *ptr;
    } while (__sync_val_compare_and_swap(ptr, oldvar, value) != oldvar);

    return oldvar;
};
#endif

#endif//MACFUNC_H_
