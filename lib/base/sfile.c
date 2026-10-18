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
#ifdef OS_WIN
    SET_CLOEXEC(_get_osfhandle(_fileno(fp)));
#else
    SET_CLOEXEC(fileno(fp));
#endif
    return fp;
}
char *readall(const char *file, size_t *lens) {
    // 只读常规文件：目录在部分文件系统上能 fopen 成功，ftell 给的是哨兵值不是长度。
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
    if (0 != fseek(fp, 0, SEEK_END)) {
        /* fclose 失败会盖掉 fseek 留下的 errno，先存后还 */
        int32_t err = errno;
        fclose(fp);
        errno = err;
        return NULL;
    }
    long sz = ftell(fp);
    /* ftell 对管道/特殊文件返回 -1，文件大小 0 也视为无效。这条不来自失败的系统调用，
       errno 里躺着的是上一次调用的陈旧值，得自己补一个 */
    if (sz <= 0) {
        fclose(fp);
        errno = EINVAL;
        return NULL;
    }
    rewind(fp);
    char *buf;
    MALLOC(buf, (size_t)sz + 1);
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        /* 短读：文件在读取过程中被截断或发生 I/O 错误；fread 不保证置 errno，同上自己补 */
        FREE(buf);
        errno = EIO;
        return NULL;
    }
    buf[sz] = '\0';
    *lens = (size_t)sz;
    return buf;
}
