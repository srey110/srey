#include "utils/utils.h"
#include "utils/strptime.h"
#include "base/structs.h"

#ifdef OS_WIN
#pragma warning(disable:4091)
#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib" )
static atomic_t _exindex = 0;
#endif

#define _MC ((1 << CHAR_BIT) - 1) //0xff
static void *_ud;
static void(*_sig_cb)(int32_t, void *);
static atomic64_t _ids = 1;
static char _path[PATH_LENS] = { 0 };
static atomic_t _path_once = 0;

#ifdef OS_WIN
static BOOL _GetImpersonationToken(HANDLE *handle) {
    if (!OpenThreadToken(GetCurrentThread(),
        TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
        TRUE,
        handle)) {
        if (ERROR_NO_TOKEN == ERRNO) {
            if (!OpenProcessToken(GetCurrentProcess(),
                TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES,
                handle)) {
                return FALSE;
            }
        } else {
            return FALSE;
        }
    }
    return TRUE;
}
static BOOL _EnablePrivilege(LPCTSTR priv, HANDLE handle, TOKEN_PRIVILEGES *privold) {
    TOKEN_PRIVILEGES tpriv;
    tpriv.PrivilegeCount = 1;
    tpriv.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValue(0, priv, &tpriv.Privileges[0].Luid)) {
        return FALSE;
    }
    DWORD dsize = sizeof(TOKEN_PRIVILEGES);
    return AdjustTokenPrivileges(handle, FALSE, &tpriv, dsize, privold, &dsize);
}
static LONG __stdcall _MiniDump(struct _EXCEPTION_POINTERS *excep) {
    char acdmp[PATH_LENS];
    SNPRINTF(acdmp, sizeof(acdmp), "%s%s%lld_%d.dmp",
             procpath(), PATH_SEPARATORSTR, nowsec(), (int32_t)ATOMIC_ADD(&_exindex, 1));
    HANDLE ptoken = NULL;
    if (!_GetImpersonationToken(&ptoken)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return EXCEPTION_CONTINUE_SEARCH;
    }
    HANDLE pdmpfile = CreateFile(acdmp,
        GENERIC_WRITE,
        FILE_SHARE_WRITE,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (INVALID_HANDLE_VALUE == pdmpfile) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return EXCEPTION_CONTINUE_SEARCH;
    }

    LONG lrtn = EXCEPTION_CONTINUE_SEARCH;
    TOKEN_PRIVILEGES tprivold;
    MINIDUMP_EXCEPTION_INFORMATION exinfo;
    exinfo.ThreadId = GetCurrentThreadId();
    exinfo.ExceptionPointers = excep;
    exinfo.ClientPointers = FALSE;
    BOOL bprienabled = _EnablePrivilege(SE_DEBUG_NAME, ptoken, &tprivold);
    BOOL bok = MiniDumpWriteDump(GetCurrentProcess(),
        GetCurrentProcessId(),
        pdmpfile,
        MiniDumpNormal,
        &exinfo,
        NULL,
        NULL);
    if (bok) {
        lrtn = EXCEPTION_EXECUTE_HANDLER;
    } else {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
    if (bprienabled) {
        (void)AdjustTokenPrivileges(ptoken, FALSE, &tprivold, 0, NULL, NULL);
    }
    CloseHandle(pdmpfile);
    TerminateProcess(GetCurrentProcess(), 0);
    return lrtn;
}
#endif
void unlimit(void) {
#ifdef OS_WIN
    SetUnhandledExceptionFilter(_MiniDump);
#else
    struct rlimit stnew;
    stnew.rlim_cur = stnew.rlim_max = RLIM_INFINITY;
    if (ERR_OK != setrlimit(RLIMIT_CORE, &stnew)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
#ifdef OS_DARWIN
    rlim_t rlmax = OPEN_MAX;
#else
    rlim_t rlmax = 65535;
#endif
    stnew.rlim_cur = stnew.rlim_max = rlmax;
    if (ERR_OK != setrlimit(RLIMIT_NOFILE, &stnew)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
    }
#endif
}
#ifdef OS_WIN
static BOOL WINAPI _sighandler(DWORD dsig) {
    switch (dsig) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        _sig_cb((int32_t)dsig, _ud);
        break;
    }
    return TRUE;
}
#else
static void _sighandler(int32_t isig) {
    _sig_cb(isig, _ud);
}
#endif
void sighandle(void(*cb)(int32_t, void *), void *data) {
    _ud = data;
    _sig_cb = cb;
#ifdef OS_WIN
    (void)SetConsoleCtrlHandler((PHANDLER_ROUTINE)_sighandler, TRUE);
#else
    signal(SIGPIPE, SIG_IGN);//若某一端关闭连接，而另一端仍然向它写数据，第一次写数据后会收到RST响应，此后再写数据，内核将向进程发出SIGPIPE信号
    signal(SIGHUP, _sighandler);//终止控制终端或进程
    signal(SIGINT, _sighandler);//键盘产生的中断(Ctrl-C)
    signal(SIGQUIT, _sighandler);//键盘产生的退出
    signal(SIGABRT, _sighandler);//异常中止
    signal(SIGTSTP, _sighandler);//ctrl+Z
    signal(SIGKILL, _sighandler);//立即结束程序
    signal(SIGTERM, _sighandler);//进程终止
    signal(SIGUSR1, _sighandler);
    signal(SIGUSR2, _sighandler);
#endif
}
uint64_t createid(void) {
    return ATOMIC64_ADD(&_ids, 1);
}
uint64_t threadid(void) {
#if defined(OS_WIN)
    return (uint64_t)GetCurrentThreadId();
#else
    return (uint64_t)pthread_self();
#endif
}
uint32_t procscnt(void) {
#if defined(OS_WIN)
    SYSTEM_INFO stinfo;
    GetSystemInfo(&stinfo);
    return (uint32_t)stinfo.dwNumberOfProcessors;
#else
    return (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
#endif
}
const char *contenttype(const char *extension) {
    typedef struct contenttype_ctx {
        const char *extension;
        const char *type;
    }contenttype_ctx;
    static contenttype_ctx typegreg[] = {
        { ".323", "text/h323" },
        { ".3g2", "video/3gpp2" },
        { ".3gp", "video/3gpp" },
        { ".3gp2", "video/3gpp2" },
        { ".3gpp", "video/3gpp" },
        { ".7z", "application/x-7z-compressed" },
        { ".aa", "audio/audible" },
        { ".aac", "audio/aac" },
        { ".aaf", "application/octet-stream" },
        { ".aax", "audio/vnd.audible.aax" },
        { ".ac3", "audio/ac3" },
        { ".aca", "application/octet-stream" },
        { ".accda", "application/msaccess.addin" },
        { ".accdb", "application/msaccess" },
        { ".accdc", "application/msaccess.cab" },
        { ".accde", "application/msaccess" },
        { ".accdr", "application/msaccess.runtime" },
        { ".accdt", "application/msaccess" },
        { ".accdw", "application/msaccess.webapplication" },
        { ".accft", "application/msaccess.ftemplate" },
        { ".acx", "application/internet-property-stream" },
        { ".addin", "text/xml" },
        { ".ade", "application/msaccess" },
        { ".adobebridge", "application/x-bridge-url" },
        { ".adp", "application/msaccess" },
        { ".adt", "audio/vnd.dlna.adts" },
        { ".adts", "audio/aac" },
        { ".afm", "application/octet-stream" },
        { ".ai", "application/postscript" },
        { ".aif", "audio/x-aiff" },
        { ".aifc", "audio/aiff" },
        { ".aiff", "audio/aiff" },
        { ".air", "application/vnd.adobe.air-application-installer-package+zip" },
        { ".amc", "application/x-mpeg" },
        { ".application", "application/x-ms-application" },
        { ".art", "image/x-jg" },
        { ".asa", "application/xml" },
        { ".asax", "application/xml" },
        { ".ascx", "application/xml" },
        { ".asd", "application/octet-stream" },
        { ".asf", "video/x-ms-asf" },
        { ".ashx", "application/xml" },
        { ".asi", "application/octet-stream" },
        { ".asm", "text/plain" },
        { ".asmx", "application/xml" },
        { ".aspx", "application/xml" },
        { ".asr", "video/x-ms-asf" },
        { ".asx", "video/x-ms-asf" },
        { ".atom", "application/atom+xml" },
        { ".au", "audio/basic" },
        { ".avi", "video/x-msvideo" },
        { ".axs", "application/olescript" },
        { ".bas", "text/plain" },
        { ".bcpio", "application/x-bcpio" },
        { ".bin", "application/octet-stream" },
        { ".bmp", "image/bmp" },
        { ".c", "text/plain" },
        { ".cab", "application/octet-stream" },
        { ".caf", "audio/x-caf" },
        { ".calx", "application/vnd.ms-office.calx" },
        { ".cat", "application/vnd.ms-pki.seccat" },
        { ".cc", "text/plain" },
        { ".cd", "text/plain" },
        { ".cdda", "audio/aiff" },
        { ".cdf", "application/x-cdf" },
        { ".cer", "application/x-x509-ca-cert" },
        { ".chm", "application/octet-stream" },
        { ".class", "application/x-java-applet" },
        { ".clp", "application/x-msclip" },
        { ".cmx", "image/x-cmx" },
        { ".cnf", "text/plain" },
        { ".cod", "image/cis-cod" },
        { ".config", "application/xml" },
        { ".contact", "text/x-ms-contact" },
        { ".coverage", "application/xml" },
        { ".cpio", "application/x-cpio" },
        { ".cpp", "text/plain" },
        { ".crd", "application/x-mscardfile" },
        { ".crl", "application/pkix-crl" },
        { ".crt", "application/x-x509-ca-cert" },
        { ".cs", "text/plain" },
        { ".csdproj", "text/plain" },
        { ".csh", "application/x-csh" },
        { ".csproj", "text/plain" },
        { ".css", "text/css" },
        { ".csv", "text/csv" },
        { ".cur", "application/octet-stream" },
        { ".cxx", "text/plain" },
        { ".dat", "application/octet-stream" },
        { ".datasource", "application/xml" },
        { ".dbproj", "text/plain" },
        { ".dcr", "application/x-director" },
        { ".def", "text/plain" },
        { ".deploy", "application/octet-stream" },
        { ".der", "application/x-x509-ca-cert" },
        { ".dgml", "application/xml" },
        { ".dib", "image/bmp" },
        { ".dif", "video/x-dv" },
        { ".dir", "application/x-director" },
        { ".disco", "text/xml" },
        { ".dll", "application/x-msdownload" },
        { ".dll.config", "text/xml" },
        { ".dlm", "text/dlm" },
        { ".doc", "application/msword" },
        { ".docm", "application/vnd.ms-word.document.macroEnabled.12" },
        { ".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
        { ".dot", "application/msword" },
        { ".dotm", "application/vnd.ms-word.template.macroEnabled.12" },
        { ".dotx", "application/vnd.openxmlformats-officedocument.wordprocessingml.template" },
        { ".dsp", "application/octet-stream" },
        { ".dsw", "text/plain" },
        { ".dtd", "text/xml" },
        { ".dtsconfig", "text/xml" },
        { ".dv", "video/x-dv" },
        { ".dvi", "application/x-dvi" },
        { ".dwf", "drawing/x-dwf" },
        { ".dwp", "application/octet-stream" },
        { ".dxr", "application/x-director" },
        { ".eml", "message/rfc822" },
        { ".emz", "application/octet-stream" },
        { ".eot", "application/octet-stream" },
        { ".eps", "application/postscript" },
        { ".etl", "application/etl" },
        { ".etx", "text/x-setext" },
        { ".evy", "application/envoy" },
        { ".exe", "application/octet-stream" },
        { ".exe.config", "text/xml" },
        { ".fdf", "application/vnd.fdf" },
        { ".fif", "application/fractals" },
        { ".filters", "Application/xml" },
        { ".fla", "application/octet-stream" },
        { ".flr", "x-world/x-vrml" },
        { ".flv", "video/x-flv" },
        { ".fsscript", "application/fsharp-script" },
        { ".fsx", "application/fsharp-script" },
        { ".generictest", "application/xml" },
        { ".gif", "image/gif" },
        { ".group", "text/x-ms-group" },
        { ".gsm", "audio/x-gsm" },
        { ".gtar", "application/x-gtar" },
        { ".gz", "application/x-gzip" },
        { ".h", "text/plain" },
        { ".hdf", "application/x-hdf" },
        { ".hdml", "text/x-hdml" },
        { ".hhc", "application/x-oleobject" },
        { ".hhk", "application/octet-stream" },
        { ".hhp", "application/octet-stream" },
        { ".hlp", "application/winhlp" },
        { ".hpp", "text/plain" },
        { ".hqx", "application/mac-binhex40" },
        { ".hta", "application/hta" },
        { ".htc", "text/x-component" },
        { ".htm", "text/html" },
        { ".html", "text/html" },
        { ".htt", "text/webviewhtml" },
        { ".hxa", "application/xml" },
        { ".hxc", "application/xml" },
        { ".hxd", "application/octet-stream" },
        { ".hxe", "application/xml" },
        { ".hxf", "application/xml" },
        { ".hxh", "application/octet-stream" },
        { ".hxi", "application/octet-stream" },
        { ".hxk", "application/xml" },
        { ".hxq", "application/octet-stream" },
        { ".hxr", "application/octet-stream" },
        { ".hxs", "application/octet-stream" },
        { ".hxt", "text/html" },
        { ".hxv", "application/xml" },
        { ".hxw", "application/octet-stream" },
        { ".hxx", "text/plain" },
        { ".i", "text/plain" },
        { ".ico", "image/x-icon" },
        { ".ics", "application/octet-stream" },
        { ".idl", "text/plain" },
        { ".ief", "image/ief" },
        { ".iii", "application/x-iphone" },
        { ".inc", "text/plain" },
        { ".inf", "application/octet-stream" },
        { ".inl", "text/plain" },
        { ".ins", "application/x-internet-signup" },
        { ".ipa", "application/x-itunes-ipa" },
        { ".ipg", "application/x-itunes-ipg" },
        { ".ipproj", "text/plain" },
        { ".ipsw", "application/x-itunes-ipsw" },
        { ".iqy", "text/x-ms-iqy" },
        { ".isp", "application/x-internet-signup" },
        { ".ite", "application/x-itunes-ite" },
        { ".itlp", "application/x-itunes-itlp" },
        { ".itms", "application/x-itunes-itms" },
        { ".itpc", "application/x-itunes-itpc" },
        { ".ivf", "video/x-ivf" },
        { ".jar", "application/java-archive" },
        { ".java", "application/octet-stream" },
        { ".jck", "application/liquidmotion" },
        { ".jcz", "application/liquidmotion" },
        { ".jfif", "image/pjpeg" },
        { ".jnlp", "application/x-java-jnlp-file" },
        { ".jpb", "application/octet-stream" },
        { ".jpe", "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".jpg", "image/jpeg" },
        { ".js", "application/x-javascript" },
        { ".jsx", "text/jscript" },
        { ".jsxbin", "text/plain" },
        { ".latex", "application/x-latex" },
        { ".library-ms", "application/windows-library+xml" },
        { ".lit", "application/x-ms-reader" },
        { ".loadtest", "application/xml" },
        { ".lpk", "application/octet-stream" },
        { ".lsf", "video/x-la-asf" },
        { ".lst", "text/plain" },
        { ".lsx", "video/x-la-asf" },
        { ".lzh", "application/octet-stream" },
        { ".m13", "application/x-msmediaview" },
        { ".m14", "application/x-msmediaview" },
        { ".m1v", "video/mpeg" },
        { ".m2t", "video/vnd.dlna.mpeg-tts" },
        { ".m2ts", "video/vnd.dlna.mpeg-tts" },
        { ".m2v", "video/mpeg" },
        { ".m3u", "audio/x-mpegurl" },
        { ".m3u8", "audio/x-mpegurl" },
        { ".m4a", "audio/m4a" },
        { ".m4b", "audio/m4b" },
        { ".m4p", "audio/m4p" },
        { ".m4r", "audio/x-m4r" },
        { ".m4v", "video/x-m4v" },
        { ".mac", "image/x-macpaint" },
        { ".mak", "text/plain" },
        { ".man", "application/x-troff-man" },
        { ".manifest", "application/x-ms-manifest" },
        { ".map", "text/plain" },
        { ".master", "application/xml" },
        { ".mda", "application/msaccess" },
        { ".mdb", "application/x-msaccess" },
        { ".mde", "application/msaccess" },
        { ".mdp", "application/octet-stream" },
        { ".me", "application/x-troff-me" },
        { ".mfp", "application/x-shockwave-flash" },
        { ".mht", "message/rfc822" },
        { ".mhtml", "message/rfc822" },
        { ".mid", "audio/mid" },
        { ".midi", "audio/mid" },
        { ".mix", "application/octet-stream" },
        { ".mk", "text/plain" },
        { ".mmf", "application/x-smaf" },
        { ".mno", "text/xml" },
        { ".mny", "application/x-msmoney" },
        { ".mod", "video/mpeg" },
        { ".mov", "video/quicktime" },
        { ".movie", "video/x-sgi-movie" },
        { ".mp2", "video/mpeg" },
        { ".mp2v", "video/mpeg" },
        { ".mp3", "audio/mpeg" },
        { ".mp4", "video/mp4" },
        { ".mp4v", "video/mp4" },
        { ".mpa", "video/mpeg" },
        { ".mpe", "video/mpeg" },
        { ".mpeg", "video/mpeg" },
        { ".mpf", "application/vnd.ms-mediapackage" },
        { ".mpg", "video/mpeg" },
        { ".mpp", "application/vnd.ms-project" },
        { ".mpv2", "video/mpeg" },
        { ".mqv", "video/quicktime" },
        { ".ms", "application/x-troff-ms" },
        { ".msi", "application/octet-stream" },
        { ".mso", "application/octet-stream" },
        { ".mts", "video/vnd.dlna.mpeg-tts" },
        { ".mtx", "application/xml" },
        { ".mvb", "application/x-msmediaview" },
        { ".mvc", "application/x-miva-compiled" },
        { ".mxp", "application/x-mmxp" },
        { ".nc", "application/x-netcdf" },
        { ".nsc", "video/x-ms-asf" },
        { ".nws", "message/rfc822" },
        { ".ocx", "application/octet-stream" },
        { ".oda", "application/oda" },
        { ".odc", "text/x-ms-odc" },
        { ".odh", "text/plain" },
        { ".odl", "text/plain" },
        { ".odp", "application/vnd.oasis.opendocument.presentation" },
        { ".ods", "application/oleobject" },
        { ".odt", "application/vnd.oasis.opendocument.text" },
        { ".one", "application/onenote" },
        { ".onea", "application/onenote" },
        { ".onepkg", "application/onenote" },
        { ".onetmp", "application/onenote" },
        { ".onetoc", "application/onenote" },
        { ".onetoc2", "application/onenote" },
        { ".orderedtest", "application/xml" },
        { ".osdx", "application/opensearchdescription+xml" },
        { ".p10", "application/pkcs10" },
        { ".p12", "application/x-pkcs12" },
        { ".p7b", "application/x-pkcs7-certificates" },
        { ".p7c", "application/pkcs7-mime" },
        { ".p7m", "application/pkcs7-mime" },
        { ".p7r", "application/x-pkcs7-certreqresp" },
        { ".p7s", "application/pkcs7-signature" },
        { ".pbm", "image/x-portable-bitmap" },
        { ".pcast", "application/x-podcast" },
        { ".pct", "image/pict" },
        { ".pcx", "application/octet-stream" },
        { ".pcz", "application/octet-stream" },
        { ".pdf", "application/pdf" },
        { ".pfb", "application/octet-stream" },
        { ".pfm", "application/octet-stream" },
        { ".pfx", "application/x-pkcs12" },
        { ".pgm", "image/x-portable-graymap" },
        { ".pic", "image/pict" },
        { ".pict", "image/pict" },
        { ".pkgdef", "text/plain" },
        { ".pkgundef", "text/plain" },
        { ".pko", "application/vnd.ms-pki.pko" },
        { ".pls", "audio/scpls" },
        { ".pma", "application/x-perfmon" },
        { ".pmc", "application/x-perfmon" },
        { ".pml", "application/x-perfmon" },
        { ".pmr", "application/x-perfmon" },
        { ".pmw", "application/x-perfmon" },
        { ".png", "image/png" },
        { ".pnm", "image/x-portable-anymap" },
        { ".pnt", "image/x-macpaint" },
        { ".pntg", "image/x-macpaint" },
        { ".pnz", "image/png" },
        { ".pot", "application/vnd.ms-powerpoint" },
        { ".potm", "application/vnd.ms-powerpoint.template.macroEnabled.12" },
        { ".potx", "application/vnd.openxmlformats-officedocument.presentationml.template" },
        { ".ppa", "application/vnd.ms-powerpoint" },
        { ".ppam", "application/vnd.ms-powerpoint.addin.macroEnabled.12" },
        { ".ppm", "image/x-portable-pixmap" },
        { ".pps", "application/vnd.ms-powerpoint" },
        { ".ppsm", "application/vnd.ms-powerpoint.slideshow.macroEnabled.12" },
        { ".ppsx", "application/vnd.openxmlformats-officedocument.presentationml.slideshow" },
        { ".ppt", "application/vnd.ms-powerpoint" },
        { ".pptm", "application/vnd.ms-powerpoint.presentation.macroEnabled.12" },
        { ".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
        { ".prf", "application/pics-rules" },
        { ".prm", "application/octet-stream" },
        { ".prx", "application/octet-stream" },
        { ".ps", "application/postscript" },
        { ".psc1", "application/PowerShell" },
        { ".psd", "application/octet-stream" },
        { ".psess", "application/xml" },
        { ".psm", "application/octet-stream" },
        { ".psp", "application/octet-stream" },
        { ".pub", "application/x-mspublisher" },
        { ".pwz", "application/vnd.ms-powerpoint" },
        { ".qht", "text/x-html-insertion" },
        { ".qhtm", "text/x-html-insertion" },
        { ".qt", "video/quicktime" },
        { ".qti", "image/x-quicktime" },
        { ".qtif", "image/x-quicktime" },
        { ".qtl", "application/x-quicktimeplayer" },
        { ".qxd", "application/octet-stream" },
        { ".ra", "audio/x-pn-realaudio" },
        { ".ram", "audio/x-pn-realaudio" },
        { ".rar", "application/octet-stream" },
        { ".ras", "image/x-cmu-raster" },
        { ".rat", "application/rat-file" },
        { ".rc", "text/plain" },
        { ".rc2", "text/plain" },
        { ".rct", "text/plain" },
        { ".rdlc", "application/xml" },
        { ".resx", "application/xml" },
        { ".rf", "image/vnd.rn-realflash" },
        { ".rgb", "image/x-rgb" },
        { ".rgs", "text/plain" },
        { ".rm", "application/vnd.rn-realmedia" },
        { ".rmi", "audio/mid" },
        { ".rmp", "application/vnd.rn-rn_music_package" },
        { ".roff", "application/x-troff" },
        { ".rpm", "audio/x-pn-realaudio-plugin" },
        { ".rqy", "text/x-ms-rqy" },
        { ".rtf", "application/rtf" },
        { ".rtx", "text/richtext" },
        { ".ruleset", "application/xml" },
        { ".s", "text/plain" },
        { ".safariextz", "application/x-safari-safariextz" },
        { ".scd", "application/x-msschedule" },
        { ".sct", "text/scriptlet" },
        { ".sd2", "audio/x-sd2" },
        { ".sdp", "application/sdp" },
        { ".sea", "application/octet-stream" },
        { ".searchConnector-ms", "application/windows-search-connector+xml" },
        { ".setpay", "application/set-payment-initiation" },
        { ".setreg", "application/set-registration-initiation" },
        { ".settings", "application/xml" },
        { ".sgimb", "application/x-sgimb" },
        { ".sgml", "text/sgml" },
        { ".sh", "application/x-sh" },
        { ".shar", "application/x-shar" },
        { ".shtml", "text/html" },
        { ".sit", "application/x-stuffit" },
        { ".sitemap", "application/xml" },
        { ".skin", "application/xml" },
        { ".sldm", "application/vnd.ms-powerpoint.slide.macroEnabled.12" },
        { ".sldx", "application/vnd.openxmlformats-officedocument.presentationml.slide" },
        { ".slk", "application/vnd.ms-excel" },
        { ".sln", "text/plain" },
        { ".slupkg-ms", "application/x-ms-license" },
        { ".smd", "audio/x-smd" },
        { ".smi", "application/octet-stream" },
        { ".smx", "audio/x-smd" },
        { ".smz", "audio/x-smd" },
        { ".snd", "audio/basic" },
        { ".snippet", "application/xml" },
        { ".snp", "application/octet-stream" },
        { ".sol", "text/plain" },
        { ".sor", "text/plain" },
        { ".spc", "application/x-pkcs7-certificates" },
        { ".spl", "application/futuresplash" },
        { ".src", "application/x-wais-source" },
        { ".srf", "text/plain" },
        { ".ssisdeploymentmanifest", "text/xml" },
        { ".ssm", "application/streamingmedia" },
        { ".sst", "application/vnd.ms-pki.certstore" },
        { ".stl", "application/vnd.ms-pki.stl" },
        { ".sv4cpio", "application/x-sv4cpio" },
        { ".sv4crc", "application/x-sv4crc" },
        { ".svc", "application/xml" },
        { ".swf", "application/x-shockwave-flash" },
        { ".t", "application/x-troff" },
        { ".tar", "application/x-tar" },
        { ".tcl", "application/x-tcl" },
        { ".testrunconfig", "application/xml" },
        { ".testsettings", "application/xml" },
        { ".tex", "application/x-tex" },
        { ".texi", "application/x-texinfo" },
        { ".texinfo", "application/x-texinfo" },
        { ".tgz", "application/x-compressed" },
        { ".thmx", "application/vnd.ms-officetheme" },
        { ".thn", "application/octet-stream" },
        { ".tif", "image/tiff" },
        { ".tiff", "image/tiff" },
        { ".tlh", "text/plain" },
        { ".tli", "text/plain" },
        { ".toc", "application/octet-stream" },
        { ".tr", "application/x-troff" },
        { ".trm", "application/x-msterminal" },
        { ".trx", "application/xml" },
        { ".ts", "video/vnd.dlna.mpeg-tts" },
        { ".tsv", "text/tab-separated-values" },
        { ".ttf", "application/octet-stream" },
        { ".tts", "video/vnd.dlna.mpeg-tts" },
        { ".txt", "text/plain" },
        { ".u32", "application/octet-stream" },
        { ".uls", "text/iuls" },
        { ".user", "text/plain" },
        { ".ustar", "application/x-ustar" },
        { ".vb", "text/plain" },
        { ".vbdproj", "text/plain" },
        { ".vbk", "video/mpeg" },
        { ".vbproj", "text/plain" },
        { ".vbs", "text/vbscript" },
        { ".vcf", "text/x-vcard" },
        { ".vcproj", "Application/xml" },
        { ".vcs", "text/plain" },
        { ".vcxproj", "Application/xml" },
        { ".vddproj", "text/plain" },
        { ".vdp", "text/plain" },
        { ".vdproj", "text/plain" },
        { ".vdx", "application/vnd.ms-visio.viewer" },
        { ".vml", "text/xml" },
        { ".vscontent", "application/xml" },
        { ".vsct", "text/xml" },
        { ".vsd", "application/vnd.visio" },
        { ".vsi", "application/ms-vsi" },
        { ".vsix", "application/vsix" },
        { ".vsixlangpack", "text/xml" },
        { ".vsixmanifest", "text/xml" },
        { ".vsmdi", "application/xml" },
        { ".vspscc", "text/plain" },
        { ".vss", "application/vnd.visio" },
        { ".vsscc", "text/plain" },
        { ".vssettings", "text/xml" },
        { ".vssscc", "text/plain" },
        { ".vst", "application/vnd.visio" },
        { ".vstemplate", "text/xml" },
        { ".vsto", "application/x-ms-vsto" },
        { ".vsw", "application/vnd.visio" },
        { ".vsx", "application/vnd.visio" },
        { ".vtx", "application/vnd.visio" },
        { ".wav", "audio/wav" },
        { ".wave", "audio/wav" },
        { ".wax", "audio/x-ms-wax" },
        { ".wbk", "application/msword" },
        { ".wbmp", "image/vnd.wap.wbmp" },
        { ".wcm", "application/vnd.ms-works" },
        { ".wdb", "application/vnd.ms-works" },
        { ".wdp", "image/vnd.ms-photo" },
        { ".webarchive", "application/x-safari-webarchive" },
        { ".webtest", "application/xml" },
        { ".wiq", "application/xml" },
        { ".wiz", "application/msword" },
        { ".wks", "application/vnd.ms-works" },
        { ".wlmp", "application/wlmoviemaker" },
        { ".wlpginstall", "application/x-wlpg-detect" },
        { ".wlpginstall3", "application/x-wlpg3-detect" },
        { ".wm", "video/x-ms-wm" },
        { ".wma", "audio/x-ms-wma" },
        { ".wmd", "application/x-ms-wmd" },
        { ".wmf", "application/x-msmetafile" },
        { ".wml", "text/vnd.wap.wml" },
        { ".wmlc", "application/vnd.wap.wmlc" },
        { ".wmls", "text/vnd.wap.wmlscript" },
        { ".wmlsc", "application/vnd.wap.wmlscriptc" },
        { ".wmp", "video/x-ms-wmp" },
        { ".wmv", "video/x-ms-wmv" },
        { ".wmx", "video/x-ms-wmx" },
        { ".wmz", "application/x-ms-wmz" },
        { ".wpl", "application/vnd.ms-wpl" },
        { ".wps", "application/vnd.ms-works" },
        { ".wri", "application/x-mswrite" },
        { ".wrl", "x-world/x-vrml" },
        { ".wrz", "x-world/x-vrml" },
        { ".wsc", "text/scriptlet" },
        { ".wsdl", "text/xml" },
        { ".wvx", "video/x-ms-wvx" },
        { ".x", "application/directx" },
        { ".xaf", "x-world/x-vrml" },
        { ".xaml", "application/xaml+xml" },
        { ".xap", "application/x-silverlight-app" },
        { ".xbap", "application/x-ms-xbap" },
        { ".xbm", "image/x-xbitmap" },
        { ".xdr", "text/plain" },
        { ".xht", "application/xhtml+xml" },
        { ".xhtml", "application/xhtml+xml" },
        { ".xla", "application/vnd.ms-excel" },
        { ".xlam", "application/vnd.ms-excel.addin.macroEnabled.12" },
        { ".xlc", "application/vnd.ms-excel" },
        { ".xld", "application/vnd.ms-excel" },
        { ".xlk", "application/vnd.ms-excel" },
        { ".xll", "application/vnd.ms-excel" },
        { ".xlm", "application/vnd.ms-excel" },
        { ".xls", "application/vnd.ms-excel" },
        { ".xlsb", "application/vnd.ms-excel.sheet.binary.macroEnabled.12" },
        { ".xlsm", "application/vnd.ms-excel.sheet.macroEnabled.12" },
        { ".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
        { ".xlt", "application/vnd.ms-excel" },
        { ".xltm", "application/vnd.ms-excel.template.macroEnabled.12" },
        { ".xltx", "application/vnd.openxmlformats-officedocument.spreadsheetml.template" },
        { ".xlw", "application/vnd.ms-excel" },
        { ".xml", "text/xml" },
        { ".xmta", "application/xml" },
        { ".xof", "x-world/x-vrml" },
        { ".xoml", "text/plain" },
        { ".xpm", "image/x-xpixmap" },
        { ".xps", "application/vnd.ms-xpsdocument" },
        { ".xrm-ms", "text/xml" },
        { ".xsc", "application/xml" },
        { ".xsd", "text/xml" },
        { ".xsf", "text/xml" },
        { ".xsl", "text/xml" },
        { ".xslt", "text/xml" },
        { ".xsn", "application/octet-stream" },
        { ".xss", "application/xml" },
        { ".xtp", "application/octet-stream" },
        { ".xwd", "image/x-xwindowdump" },
        { ".z", "application/x-compress" },
        { ".zip", "application/x-zip-compressed" },
        { NULL, NULL }
    };
    size_t exlens = strlen(extension);
    contenttype_ctx *conttype;
    for (int32_t i = 0; ;i++) {
        conttype = &typegreg[i];
        if (NULL == conttype->extension) {
            break;
        }
        if (exlens == strlen(conttype->extension)
            && 0 == _memicmp(conttype->extension, extension, exlens)) {
            return conttype->type;
        }
    }
    return "application/X-other-1";
}
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
#ifdef OS_AIX
static int32_t _get_proc(pid_t pid, struct procsinfo *info) {
    int32_t i, cnt;
    pid_t index = 0;
    struct procsinfo pinfo[16];
    while ((cnt = getprocs(pinfo, sizeof(struct procsinfo), NULL, 0, &index, 16)) > 0) {
        for (i = 0; i < cnt; i++) {
            if (SZOMB == pinfo[i].pi_state) {
                continue;
            }
            //pinfo[i].pi_comm 程序名
            if (pid == pinfo[i].pi_pid) {
                memcpy(info, &pinfo[i], sizeof(struct procsinfo));
                return ERR_OK;
            }
        }
    }
    return ERR_FAILED;
}
static int32_t _get_proc_fullpath(pid_t pid, char path[PATH_LENS]) {
    struct procsinfo pinfo;
    if (ERR_OK != _get_proc(pid, &pinfo)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    char args[ONEK];
    //args consists of a succession of strings, each terminated with a null character (ascii `\0'). 
    //Hence, two consecutive NULLs indicate the end of the list.
    if (ERR_OK != getargs(&pinfo, sizeof(struct procsinfo), args, sizeof(args))) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    if (NULL == realpath(args, path)) {
        LOG_ERROR("%s", ERRORSTR(ERRNO));
        return ERR_FAILED;
    }
    return ERR_OK;
}
#endif
static int32_t _get_procpath(char path[PATH_LENS]) {
#ifndef OS_AIX
    size_t len = PATH_LENS;
#endif
#if defined(OS_WIN)
    if (0 == GetModuleFileName(NULL, path, (DWORD)len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_LINUX)
    if (0 > readlink("/proc/self/exe", path, len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_NBSD)
    if (0 > readlink("/proc/curproc/exe", path, len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_DFBSD)
    if (0 > readlink("/proc/curproc/file", path, len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_SUN)  
    char in[64];
    SNPRINTF(in, sizeof(in), "/proc/%d/path/a.out", (uint32_t)getpid());
    if (0 > readlink(in, path, len - 1)) {
        return ERR_FAILED;
    }
#elif defined(OS_DARWIN)
    uint32_t umaclens = len;
    if (0 != _NSGetExecutablePath(path, &umaclens)) {
        return ERR_FAILED;
    }
#elif defined(OS_FBSD)
    int32_t name[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME };
    name[3] = getpid();
    if (0 != sysctl(name, 4, path, &len, NULL, 0)) {
        return ERR_FAILED;
    }
#elif defined(OS_AIX)
    if (ERR_OK != _get_proc_fullpath(getpid(), path)) {
        return ERR_FAILED;
    }
#elif defined(OS_HPUX)
    struct pst_status pst;
    if (-1 == pstat_getproc(&pst, sizeof(pst), 0, getpid())) {
        return ERR_FAILED;
    }
    if (-1 == pstat_getpathname(path, len - 1, &pst.pst_fid_text)) {
        return ERR_FAILED;
    }
#else
#error "not support."
#endif
    char* cur = strrchr(path, PATH_SEPARATOR);
    *cur = 0;
#if defined(OS_DARWIN)
    cur = strstr(path, "./");
    if (NULL != cur) {
        len = strlen(cur + 2);
        memcpy(path + (cur - path), cur + 2, len);
        len = cur - path + len;
        path[len] = 0;
    } else {
        len = strlen(path);
        if ('.' == path[len - 1]
            && PATH_SEPARATOR == path[len - 2]) {
            path[len - 2] = 0;
        }
    }
    if (PATH_SEPARATOR == path[0]
        && PATH_SEPARATOR == path[1]) {
        len = strlen(path);
        memcpy(path, path + 1, len - 1);
        path[len - 1] = 0;
    }
#endif
    return ERR_OK;
}
const char *procpath(void) {
    if (ATOMIC_CAS(&_path_once, 0, 1)) {
        ASSERTAB(ERR_OK == _get_procpath(_path), ERRORSTR(ERRNO));
    }
    return _path;
}
void timeofday(struct timeval *tv) {
#if defined(OS_WIN)
#define U64_LITERAL(n) n##ui64
#define EPOCH_BIAS U64_LITERAL(116444736000000000)
#define UNITS_PER_SEC U64_LITERAL(10000000)
#define USEC_PER_SEC U64_LITERAL(1000000)
#define UNITS_PER_USEC U64_LITERAL(10)
    union {
        FILETIME ft_ft;
        uint64_t ft_64;
    } ft;
    GetSystemTimeAsFileTime(&ft.ft_ft);
    ft.ft_64 -= EPOCH_BIAS;
    tv->tv_sec = (long)(ft.ft_64 / UNITS_PER_SEC);
    tv->tv_usec = (long)((ft.ft_64 / UNITS_PER_USEC) % USEC_PER_SEC);
#else
    (void)gettimeofday(tv, NULL);
#endif
}
char *readall(const char *file, size_t *lens) {
    FILE *fp = fopen(file, "rb");
    if (NULL == fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    *lens = ftell(fp);
    rewind(fp);
    char *buf;
    MALLOC(buf, (*lens) + 1);
    fread(buf, 1, (*lens), fp);
    fclose(fp);
    buf[*lens] = '\0';
    return buf;
}
int32_t timeoffset(void) {
    time_t now = time(NULL);
    //系统时间转换为GMT时间 再将GMT时间重新转换为系统时间
    time_t gt = mktime(gmtime(&now));
    return ((int32_t)(now - gt) + (localtime(&gt)->tm_isdst ? 3600 : 0)) / 60;
}
uint64_t nowms(void) {
    struct timeval tv;
    timeofday(&tv);
    return (uint64_t)tv.tv_usec / 1000 + (uint64_t)tv.tv_sec * 1000;
}
uint64_t nowsec(void) {
    struct timeval tv;
    timeofday(&tv);
    return (uint64_t)tv.tv_sec;
}
void sectostr(uint64_t sec, const char *fmt, char time[TIME_LENS]) {
    time_t t = (time_t)sec;
    strftime(time, TIME_LENS - 1, fmt, localtime(&t));
}
void mstostr(uint64_t ms, const char *fmt, char time[TIME_LENS]) {
    time_t t = (time_t)(ms / 1000);
    strftime(time, TIME_LENS - 1, fmt, localtime(&t));
    size_t uilen = strlen(time);
    SNPRINTF(time + uilen, TIME_LENS - uilen, " %03d", (int32_t)(ms % 1000));
}
uint64_t strtots(const char *time, const char *fmt) {
    struct tm dttm;
    if (NULL == _strptime(time, fmt, &dttm)) {
        return 0;
    }
    return (uint64_t)mktime(&dttm);
}
void fill_timespec(struct timespec *timeout, uint32_t ms) {
    if (ms >= 1000) {
        timeout->tv_sec = ms / 1000;
        timeout->tv_nsec = (long)(ms - timeout->tv_sec * 1000) * (1000 * 1000);
    } else {
        timeout->tv_sec = 0;
        timeout->tv_nsec = ms * (1000 * 1000);
    }
}
uint64_t hash(const char *buf, size_t len) {
    uint64_t rtn = 0;
    for (; len > 0; --len) {
        rtn = (rtn * 131) + *buf++;
    }
    return rtn;
}
void *memichr(const void *ptr, int32_t val, size_t maxlen) {
    char *buf = (char *)ptr;
    val = tolower(val);
    while (maxlen--) {
        if (tolower(*buf) == val) {
            return (void *)buf;
        }
        buf++;
    }
    return NULL;
}
#ifndef OS_WIN
int32_t _memicmp(const void *ptr1, const void *ptr2, size_t lens) {
    int32_t i = 0;
    char *buf1 = (char *)ptr1;
    char *buf2 = (char *)ptr2;
    while (tolower(*buf1) == tolower(*buf2)
        && i < (int32_t)lens) {
        buf1++;
        buf2++;
        i++;
    }
    if (i == (int32_t)lens) {
        return 0;
    } else {
        if (*buf1 > *buf2) {
            return 1;
        } else {
            return -1;
        }
    }
}
#endif
void *memstr(int32_t ncs, const void *ptr, size_t plens, const void *what, size_t wlen) {
    if (NULL == ptr
        || NULL == what
        || 0 == plens
        || 0 == wlen
        || wlen > plens) {
        return NULL;
    }
    chr_func chr;
    cmp_func cmp;
    if (0 == ncs) {
        chr = memchr;
        cmp = memcmp;
    } else {
        chr = memichr;
        cmp = _memicmp;
    }
    char *pos;
    char *wt = (char *)what;
    char *cur = (char *)ptr;
    do {
        pos = chr(cur, wt[0], plens - (size_t)(cur - (char*)ptr));
        if (NULL == pos
            || plens - (size_t)(pos - (char*)ptr) < wlen) {
            return NULL;
        }
        if (0 == cmp(pos, what, wlen)) {
            return (void *)pos;
        }
        cur = pos + 1;
    } while (plens - (size_t)(cur - (char*)ptr) >= wlen);
    return NULL;
}
void *skipempty(const void *ptr, size_t plens) {
    char *cur = (char *)ptr;
    while (' ' == *cur
        && (size_t)(cur - (char *)ptr) < plens) {
        cur++;
    }
    if (cur - (char *)ptr == plens) {
        return NULL;
    }
    return cur;
}
char *strupper(char *str){
    if (NULL == str) {
        return NULL;
    }
    char* p = str;
    while (*p != '\0') {
        if (*p >= 'a'
            && *p <= 'z') {
            *p &= ~0x20;
        }
        ++p;
    }
    return str;
}
char *strlower(char *str) {
    if (NULL == str) {
        return NULL;
    }
    char *p = str;
    while (*p != '\0') {
        if (*p >= 'A' && *p <= 'Z') {
            BIT_SET(*p, 0x20);
        }
        ++p;
    }
    return str;
}
char* strreverse(char* str) {
    if (NULL == str) {
        return NULL;
    }
    char* b = str;
    char* e = str;
    while (*e) { 
        ++e;
    }
    --e;
    char tmp;
    while (e > b) {
        tmp = *e;
        *e = *b;
        *b = tmp;
        --e;
        ++b;
    }
    return str;
}
int32_t randrange(int32_t min, int32_t max) {
    static uint32_t seed = 0;
    ASSERTAB(max > min, "rand range max must big than min.");
    if (0 == seed) {
        struct timeval tv;
        timeofday(&tv);
        seed = (uint32_t)(tv.tv_usec / 1000);
        srand(seed);
    }
    return min + rand() % (max - min + 1);
}
char *randstr(char *buf, size_t len) {
    static char characters[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U',
        'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p',
        'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    };
    size_t i = 0;
    for (; i < len; i++) {
        buf[i] = characters[randrange(0, sizeof(characters) - 1)];
    }
    buf[i] = '\0';
    return buf;
}
static const char hex_char[16] = {
    '0', '1', '2', '3',
    '4', '5', '6', '7',
    '8', '9', 'A', 'B',
    'C', 'D', 'E', 'F'
};
char *tohex(const void *buf, size_t len, char *out) {
    size_t j = 0;
    unsigned char *p = (unsigned char *)buf;
    for (size_t i = 0; i < len; ++i) {
        out[j] = hex_char[(p[i] / 16)];
        ++j;
        out[j] = hex_char[(p[i] % 16)];
        ++j;
    }
    out[j] = '\0';
    return out;
}
buf_ctx *split(const void *ptr, size_t plens, const void *sep, size_t seplens, size_t *n) {
    if (NULL == ptr
        || 0 == plens) {
        return NULL;
    }
    *n = 0;
    buf_ctx *buf;
    if (NULL == sep
        || 0 == seplens) {
        MALLOC(buf, sizeof(buf_ctx));
        buf[*n].data = (void *)ptr;
        buf[*n].lens = plens;
        (*n)++;
        return buf;
    }
    size_t size, total = 32;
    MALLOC(buf, sizeof(buf_ctx) * total);
    char *pos;
    char *cur = (char *)ptr;
    do {
        pos = memstr(0, cur, plens, sep, seplens);
        if (*n >= total) {
            total *= 2;
            buf = REALLOC(buf, buf, sizeof(buf_ctx) * total);
        }
        if (NULL != pos) {
            size = (size_t)(pos - cur);
            if (size > 0) {
                buf[*n].data = (void *)cur;
                buf[*n].lens = size;
            } else {
                buf[*n].data = NULL;
                buf[*n].lens = 0;
            }
            (*n)++;
            cur += (size + seplens);
            plens -= (size + seplens);
            //以分隔符结尾
            if (0 == plens) {
                if (*n >= total) {
                    ++total;
                    buf = REALLOC(buf, buf, sizeof(buf_ctx) * total);
                }
                buf[*n].data = NULL;
                buf[*n].lens = 0;
                (*n)++;
            }
        } else {
            buf[*n].data = (void *)cur;
            buf[*n].lens = plens;
            (*n)++;
        }
    } while (NULL != pos && plens > 0);
    return buf;
}
char *_format_va(const char *fmt, va_list args) {
    int32_t rtn;
    size_t size = 256;
    char *pbuff;
    MALLOC(pbuff, size);
    while (1) {
        rtn = vsnprintf(pbuff, size, fmt, args);
        if (rtn < 0) {
            FREE(pbuff);
            return NULL;
        }
        if (rtn >= 0
            && rtn < (int32_t)size) {
            return pbuff;
        }
        size = rtn + 1;
        REALLOC(pbuff, pbuff, size);
    }
}
char *format_va(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *buf = _format_va(fmt, args);
    va_end(args);
    return buf;
}
static const union {
    int32_t dummy;
    int8_t little;  /* true if machine is little endian */
} nativeendian = { 1 };
int32_t is_little(void) {
    return nativeendian.little;
}
void pack_integer(char *buf, uint64_t val, int32_t size, int32_t islittle) {
    buf[islittle ? 0 : size - 1] = (int8_t)(val & _MC);
    for (int32_t i = 1; i < size; i++) {
        val >>= CHAR_BIT;
        buf[islittle ? i : size - 1 - i] = (int8_t)(val & _MC);
    }
}
int64_t unpack_integer(const char *buf, int32_t size, int32_t islittle, int32_t issigned) {
    uint64_t rtn = 0;
    int32_t limit = (size <= sizeof(uint64_t)) ? size : sizeof(uint64_t);
    for (int32_t i = limit - 1; i >= 0; i--) {
        rtn <<= CHAR_BIT;
        rtn |= (uint64_t)(uint8_t)buf[islittle ? i : size - 1 - i];
    }
    if (size < sizeof(uint64_t)) {
        if (issigned) {
            uint64_t mask = 1llu << (size * CHAR_BIT - 1);
            rtn = ((rtn ^ mask) - mask);
        }
    }
    return (int64_t)rtn;
}
static void _copy_with_endian(char *dest, const char *src, size_t size, int32_t islittle) {
    if (islittle == is_little()) {
        memcpy(dest, src, size);
    } else {
        dest += size - 1;
        while (0 != size--) {
            *(dest--) = *(src++);
        }
    }
}
void pack_float(char *buf, float val, int32_t islittle) {
    _copy_with_endian(buf, (const char *)&val, sizeof(val), islittle);
}
float unpack_float(const char *buf, int32_t islittle) {
    float rtn;
    _copy_with_endian((char *)&rtn, buf, sizeof(rtn), islittle);
    return rtn;
}
void pack_double(char *buf, double val, int32_t islittle) {
    _copy_with_endian(buf, (const char *)&val, sizeof(val), islittle);
}
double unpack_double(const char *buf, int32_t islittle) {
    double rtn;
    _copy_with_endian((char *)&rtn, buf, sizeof(rtn), islittle);
    return rtn;
}
#if !defined(OS_WIN) && !defined(OS_DARWIN) && !defined(OS_AIX)
uint64_t ntohll(uint64_t val) {
    if (!is_little()) {
        return val;
    }
    uint64_t rtn;
    pack_integer((char *)&rtn, val, (int32_t)sizeof(uint64_t), 0);
    return rtn;
}
uint64_t htonll(uint64_t val) {
    if (!is_little()) {
        return val;
    }
    uint64_t rtn;
    pack_integer((char *)&rtn, val, (int32_t)sizeof(uint64_t), 0);
    return rtn;
}
#endif
