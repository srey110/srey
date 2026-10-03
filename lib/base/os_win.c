#include "base/os.h"
#include "base/macro_util.h"

// os_win.h 里声明、只在 Windows 上有实现的函数；别的平台整个文件编成空的
#if defined(OS_WIN)

#define _ERRSTR_LENS 4096 // _fmterror 返回缓冲的字节数
TLS_DEFINE(char, _errstr, _ERRSTR_LENS)// 每线程一份，免得多线程互相覆盖

const char *_fmterror(DWORD error) {
    char *err = NULL;
    if (0 == FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            NULL,
                            error,
                            MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                            (LPTSTR)&err,
                            0,
                            NULL)) {
        return "FormatMessageA error.";
    }
    char *errstr = _errstr_tls();
    size_t ilens = strlen(err);
    ilens = ilens >= _ERRSTR_LENS ? _ERRSTR_LENS - 1 : ilens;
    memcpy(errstr, err, ilens);
    // FormatMessageA 的文本自带结尾 CRLF，留着每条错误日志后面都多一个空行
    while (ilens > 0 && ('\r' == errstr[ilens - 1] || '\n' == errstr[ilens - 1])) {
        ilens--;
    }
    errstr[ilens] = '\0';
    LocalFree(err);
    return errstr;
}

#endif
