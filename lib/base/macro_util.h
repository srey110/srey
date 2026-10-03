#ifndef MACRO_UTIL_H_
#define MACRO_UTIL_H_

#include "base/os.h"

// 通用宏与常量，只依赖 os.h。汇总头是 base.h：base 之外一律 include 它，base 里的头按需点名引这里

#define ONEK                 1024 // 1K 字节
#define TIME_LENS            128 // 时间字符串缓冲区长度
#define HOST_LENS            256 // 主机名缓冲区长度
#define IP_LENS              64 // IP 地址字符串缓冲区长度
#define UUID_LENS            16 // UUID 字节长度
#define INVALID_FD           -1 // 无效文件描述符
#define INVALID_INDEX        -1 // 无效下标
#define FLAG_CRLF            "\r\n" // HTTP/文本协议行结束符
#define CRLF_SIZE            2 // CRLF 字节数

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a))) // 获取静态数组元素个数
#define EMPTYSTR(str) ((NULL == (str)) || ('\0' == *(const char *)(str))) // 判断字符串是否为空
#define EMPTYPTR(ptr, lens) ((NULL == (ptr)) || (0 == (lens)))
// s 向上取整,结果恒为 size_t。掩码 ~(n-1) 只在 n 是 2 的幂时才等价于"取 n 的整数倍",
// n 取别的值（比如 6）结果不会是 n 的倍数；但任何 n 下结果都 >= s（被清掉的低位至多 n-1）
#define ROUND_UP(s, n) (((s) + (n) - 1) & (~((size_t)(n) - 1)))
#define CONCAT2(a, b) a b // 拼接两个字符串字面量
#define CONCAT3(a, b, c) a b c // 拼接三个字符串字面量
#define TOSTR_(x) #x // TOSTR 的内层，直接用不会先展开宏
#define TOSTR(x) TOSTR_(x) // 宏展开后再变成字符串字面量(如 TOSTR(__LINE__))
#ifndef offsetof
    #define offsetof(type, field) ((size_t)(&((type *)0)->field)) // 获取结构体字段偏移量
#endif//offsetof
#define UPCAST(ptr, type, field) ((type *)(((char*)(ptr)) - offsetof(type, field))) // 通过成员指针还原外层结构体指针
#define BIT_SET(status, flag)    ((status) |= (flag)) // 设置位标志
#define BIT_CHECK(status, flag)  ((status) & (flag)) // 检查位标志是否已设置
#define BIT_REMOVE(status, flag) ((status) &= ~(flag)) // 清除位标志
#define BIT_GETN(x, n)           (((x) >> (n)) & 1u) // 获取第 n 位的值
// 将 x 的第 n 位设为 val 的最低位；x、n 被多次求值，须传入无副作用表达式（MSVC 要 VS 17.9 起才有 __typeof__，工程没钉工具集版本，故不拿它消多次求值）
#define BIT_SETN(x, n, val) ((x) = (((x) & ~((uint64_t)1 << (n))) | (((uint64_t)(val) & 1) << (n))))
#define ZERO(name, len) memset(name, 0, len) // 将内存区域清零
// 线程局部变量一律经它定义(mk.sh 拦裸写)：协程会在别的 worker 线程上恢复，而编译器认定 TLS 地址
// 在函数内不变、会复用到挂起之后，醒来读写的就是原线程那份。name##_tls() 经 volatile 函数指针取本线程
// 那份的首地址(n 个 T，只能零初始化)，每次调用都重新求；拿到的指针和读出的值都不许跨可能挂起协程的调用继续用
#define TLS_DEFINE(T, name, n) \
    static THREAD_LOCAL T name##_raw_[n]; \
    /* 取本线程那份的首地址，只许经 name##_tls 调 */ \
    static T *name##_addr_(void) { \
        return name##_raw_; \
    } \
    /* 访问入口，每次调用都在当前线程上现取地址；volatile 承重，别改成直接调 name##_addr_ */ \
    static inline T *name##_tls(void) { \
        T *(*volatile name##_fn_)(void) = name##_addr_; \
        return name##_fn_(); \
    }
// 关闭 socket 并将句柄置为无效值
#define CLOSE_SOCK(fd)\
    do {\
        if (INVALID_SOCK != fd) {\
            SOCK_CLOSE(fd);\
            fd = INVALID_SOCK;\
        }\
    } while(0)
// 安全地对指针解引用赋值（ptr 为 NULL 时不操作）
#define SET_PTR(ptr, val)\
    do {\
        if (NULL != (ptr)) {\
            (*(ptr)) = (val);\
        }\
    } while(0)

#endif//MACRO_UTIL_H_
