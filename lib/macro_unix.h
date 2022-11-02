#ifndef MACRO_UNIX_H_
#define MACRO_UNIX_H_

#include "os.h"

#ifndef OS_WIN

#define PATH_SEPARATOR '/'
#define PATH_SEPARATORSTR "/"
#if defined(PATH_MAX)
    #define PATH_LENS PATH_MAX
#elif defined(MAXPATHLEN)
    #define PATH_LENS MAXPATHLEN
#else
    #define PATH_LENS 260
#endif

#define SOCKET int
#define INVALID_SOCK -1

#if EAGAIN == EWOULDBLOCK
    #define IS_EAGAIN(e) (EAGAIN == (e))
#else
    #define IS_EAGAIN(e) (EAGAIN == (e) || EWOULDBLOCK == (e))
#endif
#define ERR_RW_RETRIABLE(e)	((e) == EINTR || IS_EAGAIN(e))
#define ERR_CONNECT_RETRIABLE(e) ((e) == EINTR || (e) == EINPROGRESS)
#define ERR_ACCEPT_RETRIABLE(e)	((e) == EINTR || IS_EAGAIN(e) || (e) == ECONNABORTED)
#define ERR_CONNECT_REFUSED(e) ((e) == ECONNREFUSED)
#define STRCMP strcasecmp
#define STRNCMP strncasecmp
#define STRTOK strtok_r
#define SNPRINTF snprintf
#define SWPRINTF swprintf
#define STRNCPY strncpy
#define ITOA itoa
#define STAT stat
#define USLEEP(us) usleep(us)
#define MSLEEP(ms) usleep(ms * 1000)
#define TIMEB timeb
#define FTIME ftime
#define ACCESS access
#define MKDIR(path) mkdir(path, S_IRUSR|S_IWUSR)
#define SOCK_CLOSE close
#define ERRNO errno
#define ERRORSTR(errcode) strerror(errcode)

#if defined(OS_SUN)
    typedef uint32_t atomic_t;
    typedef uint64_t atomic64_t;
    //uint32_t atomic_add_32_nv(volatile uint32_t *target, int32_t delta); return the new value of target.
    #define ATOMIC_ADD(ptr, val) atomic_add_32_nv(ptr, val)
    //uint32_t atomic_swap_32(volatile uint32_t *target, uint32_t newval);  return the old of *target.
    #define ATOMIC_SET(ptr, val) atomic_swap_32(ptr, val)
    //uint32_t atomic_cas_32(volatile uint32_t *target, uint32_t cmp, uint32_t newval);
    #define ATOMIC_CAS(ptr, oldval, newval) (atomic_cas_32(ptr, oldval, newval) == oldval)
    #define ATOMIC64_ADD(ptr, val) atomic_add_64_nv(ptr, val)
    #define ATOMIC64_SET(ptr, val) atomic_swap_64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) (atomic_cas_64(ptr, oldval, newval) == oldval)
    #define ATOMICPTR_CAS(ptr, oldval, newval) (atomic_cas_ptr(ptr, oldval, newval) == oldval)
#else
    typedef uint32_t atomic_t;
    typedef uint64_t atomic64_t;
    static inline atomic_t _fetchandset(volatile atomic_t *ptr, atomic_t value)
    {
        atomic_t oldvar;
        do
        {
            oldvar = *ptr;
        } while (!__sync_bool_compare_and_swap(ptr, oldvar, value));
        return oldvar;
    };
    static inline atomic64_t _fetchandset64(volatile atomic64_t *ptr, atomic64_t value)
    {
        atomic64_t oldvar;
        do
        {
            oldvar = *ptr;
        } while (!__sync_bool_compare_and_swap(ptr, oldvar, value));
        return oldvar;
    };
    //type __sync_fetch_and_add (type *ptr, type value, ...)//·µ»Ø¾ÉÖµ
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) _fetchandset(ptr, val)
    //bool __sync_bool_compare_and_swap (type *ptr, type oldval type newval); 
    #define ATOMIC_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC64_SET(ptr, val) _fetchandset64(ptr, val) 
    #define ATOMIC64_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMICPTR_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
#endif

#endif
#endif//MACRO_UNIX_H_