#include "base/sfile.h"
#include "base/err.h"
#include "base/macro_util.h"
#include "base/memory.h"

int32_t isfile(const char *file) {
    struct FSTAT st;
    if (ERR_OK != FSTAT(file, &st)) {
        return ERR_FAILED;
    }
#if defined(OS_WIN)
    if (BIT_CHECK(st.st_mode, _S_IFREG)) {
        return ERR_OK;
    }
    return ERR_FAILED;
#else    
    return S_ISREG(st.st_mode) ? ERR_OK : ERR_FAILED;
#endif    
}
int32_t isdir(const char *path) {
    struct FSTAT st;
    if (ERR_OK != FSTAT(path, &st)) {
        return ERR_FAILED;
    }
#if defined(OS_WIN)
    if (BIT_CHECK(st.st_mode, _S_IFDIR)) {
        return ERR_OK;
    }
    return ERR_FAILED;
#else
    return S_ISDIR(st.st_mode) ? ERR_OK : ERR_FAILED;
#endif
}
int64_t filesize(const char *file) {
    struct FSTAT st;
    if (ERR_OK != FSTAT(file, &st)) {
        return ERR_FAILED;
    }
    return st.st_size;
}
uint64_t file_mtime(const char *file) {
    struct FSTAT st;
    if (ERR_OK != FSTAT(file, &st)) {
        return 0;
    }
    return (uint64_t)st.st_mtime;
}
FILE *fopen_cloexec(const char *file, const char *mode) {
    FILE *fp = fopen(file, mode);
    if (NULL == fp) {
        return NULL;
    }
    FSET_CLOEXEC(fp);
    return fp;
}
// 按已打开的句柄取长度(64 位，>=2GB 也取得到)；失败返回 -1，errno 由 fstat 留下
static int64_t _filesize_fp(FILE *fp) {
    struct FSTAT st;
    if (0 != FSTAT_FD(FILENO(fp), &st)) {
        return -1;
    }
    return (int64_t)st.st_size;
}
char *readall(const char *file, size_t *lens) {
    // 只读常规文件：目录在部分文件系统上也能 fopen 成功，读出来的不是文件内容。
    // 不是常规文件时 errno 补 EINVAL(同空文件)，stat 失败则留它自己的 errno
    errno = 0;
    if (ERR_OK != isfile(file)) {
        if (0 == errno) {
            errno = EINVAL;
        }
        return NULL;
    }
    FILE *fp = fopen_cloexec(file, "rb");
    if (NULL == fp) {
        return NULL;
    }
    // 长度按已打开的句柄取，不按路径 stat：stat 与 open 之间文件可能被替换或追加，
    // 那样 fread 只读到前缀却照样报成功
    int64_t sz = _filesize_fp(fp);
    if (sz <= 0
        || (uint64_t)sz > SIZE_MAX - 1) {
        fclose(fp);
        errno = EINVAL;
        return NULL;
    }
    char *buf;
    MALLOC(buf, (size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    // 读满后再探一个字节：打开之后被追加过就说明拿到的不是全部
    int32_t more = (EOF != fgetc(fp)) ? 1 : 0;
    fclose(fp);
    if (got != (size_t)sz
        || 0 != more) {
        /* 短读是读取过程中被截断或 I/O 错误，多出的是期间被追加过；两者都不算读全 */
        FREE(buf);
        errno = EIO;
        return NULL;
    }
    buf[sz] = '\0';
    *lens = (size_t)sz;
    return buf;
}
