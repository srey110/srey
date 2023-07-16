#ifndef MACRO_UNIX_H_
#define MACRO_UNIX_H_

#include "os.h"

#ifndef OS_WIN

#define DLL_EXNAME "so"
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
#define STRCMP strcasecmp
#define STRNCMP strncasecmp
#define STRTOK strtok_r
#define SNPRINTF snprintf
#define SWPRINTF swprintf
#define STRNCPY strncpy
#define ITOA itoa
#define FSTAT stat
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
    static inline atomic_t _fetchandadd(atomic_t *ptr, atomic_t val) {
        return atomic_add_32_nv((volatile atomic_t *)ptr, val) - val;
    }
    static inline atomic64_t _fetchandadd64(atomic64_t *ptr, atomic64_t val) {
        return atomic_add_64_nv((volatile atomic64_t *)ptr, val) - val;
    }
    //uint32_t atomic_add_32_nv(volatile uint32_t *target, int32_t delta); return the new value of target.
    #define ATOMIC_ADD(ptr, val) _fetchandadd(ptr, val)
    //uint32_t atomic_swap_32(volatile uint32_t *target, uint32_t newval);  return the old of *target.
    #define ATOMIC_SET(ptr, val) atomic_swap_32((volatile atomic_t *)ptr, val)
    //uint32_t atomic_cas_32(volatile uint32_t *target, uint32_t cmp, uint32_t newval);
    #define ATOMIC_CAS(ptr, oldval, newval) (atomic_cas_32((volatile atomic_t *)ptr, oldval, newval) == oldval)
    #define ATOMIC64_ADD(ptr, val) _fetchandadd64(ptr, val)
    #define ATOMIC64_SET(ptr, val) atomic_swap_64((volatile atomic64_t *)ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) (atomic_cas_64((volatile atomic64_t *)ptr, oldval, newval) == oldval)
#elif defined(OS_AIX)
    typedef int32_t atomic_t;
    typedef long atomic64_t;
    static inline atomic_t _fetchandset(atomic_t *ptr, atomic_t value) {
        atomic_t oldval;
        do {
            oldval = *ptr;
        } while (!compare_and_swap(ptr, &oldval, value));
        return oldval;
    };
    static inline atomic_t _fetchandset64(atomic64_t *ptr, atomic64_t value) {
        atomic64_t oldval;
        do {
            oldval = *ptr;
        } while (!compare_and_swaplp(ptr, &oldval, value));
        return oldval;
    };
    static inline int32_t _aix_cas(atomic_t *ptr, atomic_t oldval, atomic_t newval) {
        return compare_and_swap(ptr, &oldval, newval);
    };
    static inline int32_t _aix_cas64(atomic64_t *ptr, atomic64_t oldval, atomic64_t newval) {
        return compare_and_swaplp(ptr, &oldval, newval);
    };
    #define ATOMIC_ADD(ptr, val) fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) _fetchandset(ptr, val)
    #define ATOMIC_CAS(ptr, oldval, newval) _aix_cas(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) fetch_and_addlp(ptr, val)
    #define ATOMIC64_SET(ptr, val) _fetchandset64(ptr, val)
    #define ATOMIC64_CAS(ptr, oldval, newval) _aix_cas64(ptr, oldval, newval)
#else
    typedef uint32_t atomic_t;
    typedef uint64_t atomic64_t;
    static inline atomic_t _fetchandset(atomic_t *ptr, atomic_t value) {
        atomic_t oldval;
        do {
            oldval = *ptr;
        } while (!__sync_bool_compare_and_swap(ptr, oldval, value));
        return oldval;
    };
    static inline atomic64_t _fetchandset64(atomic64_t *ptr, atomic64_t value) {
        atomic64_t oldval;
        do {
            oldval = *ptr;
        } while (!__sync_bool_compare_and_swap(ptr, oldval, value));
        return oldval;
    };
    //type __sync_fetch_and_add (type *ptr, type value, ...)//·µ»Ø¾ÉÖµ
    #define ATOMIC_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC_SET(ptr, val) _fetchandset(ptr, val)
    //bool __sync_bool_compare_and_swap (type *ptr, type oldval type newval); 
    #define ATOMIC_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
    #define ATOMIC64_ADD(ptr, val) __sync_fetch_and_add(ptr, val)
    #define ATOMIC64_SET(ptr, val) _fetchandset64(ptr, val) 
    #define ATOMIC64_CAS(ptr, oldval, newval) __sync_bool_compare_and_swap(ptr, oldval, newval)
#endif

#endif
#endif//MACRO_UNIX_H_
