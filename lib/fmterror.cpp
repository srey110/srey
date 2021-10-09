#include "fmterror.h"

#ifdef OS_WIN
std::string _fmterror(DWORD error)
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

    std::string strerr(perror);
    LocalFree(perror);

    return strerr;
}
#endif


