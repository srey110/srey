#include "startup.h"
#include "cjson/cJSON.h"

srey_ctx *srey = NULL;
static FILE *logstream = NULL;
static mutex_ctx muexit;
static cond_ctx condexit;
typedef struct config_ctx {
    uint8_t loglv;
    uint8_t logfile;
    uint16_t nnet;
    uint16_t nworker;
    uint16_t interval;
    uint16_t threshold;
    size_t stack_size;
    char fmt[64];
}config_ctx;
static char *_config_read(void) {
    char config[PATH_LENS] = { 0 };
    SNPRINTF(config, sizeof(config) - 1, "%s%s%s%s%s",
             procpath(), PATH_SEPARATORSTR, "configs", PATH_SEPARATORSTR, "config.json");
    FILE *file = fopen(config, "r");
    if (NULL == file) {
        LOG_WARN("%s", ERRORSTR(errno));
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    size_t lens = ftell(file);
    rewind(file);
    char *info;
    CALLOC(info, 1, lens + 1);
    fread(info, 1, lens, file);
    fclose(file);
    return info;
}
static void _parse_config(config_ctx *cnf) {
    char *config = _config_read();
    if (NULL == config) {
        return;
    }
    cJSON *json = cJSON_Parse(config);
    FREE(config);
    if (NULL == json) {
        const char *erro = cJSON_GetErrorPtr();
        if (NULL != erro) {
            LOG_WARN("%s", erro);
        }
        return;
    }
    cJSON *val = cJSON_GetObjectItem(json, "nnet");
    if (cJSON_IsNumber(val)) {
        cnf->nnet = (uint16_t)val->valueint;
    }
    val = cJSON_GetObjectItem(json, "nworker");
    if (cJSON_IsNumber(val)) {
        cnf->nworker = (uint16_t)val->valueint;
    }
    val = cJSON_GetObjectItem(json, "stacksize");
    if (cJSON_IsNumber(val)) {
        cnf->stack_size = (size_t)val->valueint;
    }
    val = cJSON_GetObjectItem(json, "interval");
    if (cJSON_IsNumber(val)) {
        cnf->interval = (uint16_t)val->valueint;
    }
    val = cJSON_GetObjectItem(json, "threshold");
    if (cJSON_IsNumber(val)) {
        cnf->threshold = (uint16_t)val->valueint;
    }
    val = cJSON_GetObjectItem(json, "loglv");
    if (cJSON_IsNumber(val)) {
        cnf->loglv = (uint8_t)val->valueint;
    }
    val = cJSON_GetObjectItem(json, "logfile");
    if (cJSON_IsNumber(val)) {
        cnf->logfile = (uint8_t)val->valueint;
    }
    val = cJSON_GetObjectItem(json, "namefmt");
    if (cJSON_IsString(val)) {
        size_t flen = strlen(val->valuestring);
        if (flen < sizeof(cnf->fmt)) {
            memcpy(cnf->fmt, val->valuestring, flen);
            cnf->fmt[flen] = '\0';
        } else {
            PRINT("log file name format too long.");
        }
    }
    cJSON_Delete(json);
}
static void _open_log(const char *fmt) {
    char logfile[PATH_LENS] = { 0 };
    SNPRINTF(logfile, sizeof(logfile) - 1, "%s%s%s%s", procpath(), PATH_SEPARATORSTR, "logs", PATH_SEPARATORSTR);
    if (ERR_OK != ACCESS(logfile, 0)) {
        if (ERR_OK != MKDIR(logfile)) {
            return;
        }
    }
    size_t lens = strlen(logfile);
    char time[TIME_LENS] = { 0 };
    nowtime(fmt, time);
    SNPRINTF((char*)logfile + lens, sizeof(logfile) - lens - 1, "%s%s", time, ".log");
    logstream = fopen(logfile, "a");
    if (NULL != logstream) {
        log_handle(logstream);
    }
}
static int32_t service_exit(void) {
    srey_free(srey);
    mutex_free(&muexit);
    cond_free(&condexit);
    _memcheck();
    if (NULL != logstream) {
        fclose(logstream);
    }
    return ERR_OK;
}
static void _config_init(config_ctx *config) {
    ZERO(config, sizeof(config_ctx));
    config->loglv = LOGLV_DEBUG;
    config->logfile = 1;
    config->nnet = 1;
    config->nworker = 2;
    const char *fmt = "%Y-%m-%d %H-%M-%S";
    size_t flen = strlen(fmt);
    memcpy(config->fmt, fmt, flen);
    config->fmt[flen] = '\0';
}
static int32_t service_init(void) {
    config_ctx config;
    _config_init(&config);
    _parse_config(&config);
    log_setlv((LOG_LEVEL)config.loglv);
    if (0 != config.logfile) {
        _open_log(config.fmt);
    }
    unlimit();
    mutex_init(&muexit);
    cond_init(&condexit);
    srey = srey_init(config.nnet, config.nworker, config.stack_size, config.interval, config.threshold);
    if (ERR_OK != task_startup(srey)) {
        service_exit();
        return ERR_FAILED;
    }
    return ERR_OK;
}
static void _on_sigcb(int32_t sig, void *arg) {
    LOG_INFO("catch sign: %d", sig);
    cond_signal(&condexit);
}
static int32_t service_hug(void) {
    sighandle(_on_sigcb, NULL);
    int32_t rtn = service_init();
    if (ERR_OK == rtn) {
        mutex_lock(&muexit);
        cond_wait(&condexit, &muexit);
        mutex_unlock(&muexit);
        service_exit();
    }
    return rtn;
}
#ifdef OS_WIN
    //#include "vld.h"
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "lib.lib")
    #if WITH_SSL
        #ifdef ARCH_X64
            #pragma comment(lib, "libcrypto_x64.lib")
            #pragma comment(lib, "libssl_x64.lib")
        #else
            #pragma comment(lib, "libcrypto.lib")
            #pragma comment(lib, "libssl.lib")
        #endif
    #endif

#define WINSV_STOP_TIMEOUT       30 * 1000      //windows 服务停止超时时间
#define WINSV_START_TIMEOUT      30 * 1000      //windows 服务启动超时时间

typedef WINADVAPI BOOL(WINAPI *_csd_t)(SC_HANDLE, DWORD, LPCVOID);
typedef int32_t(*_wsv_cb)(void);
static int32_t _wsv_initbasic(void);
static _wsv_cb initcbs[] = { _wsv_initbasic, service_init, NULL };
static _wsv_cb exitcbs[] = { service_exit, NULL };
static SERVICE_STATUS_HANDLE psvstatus;
static SERVICE_STATUS svstatus;

static long _wsv_exception(struct _EXCEPTION_POINTERS *exp) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    return EXCEPTION_EXECUTE_HANDLER;
}
static int32_t _wsv_initbasic(void) {
    HANDLE curproc = GetCurrentProcess();
    SetPriorityClass(curproc, HIGH_PRIORITY_CLASS);
    SetThreadPriority(curproc, THREAD_PRIORITY_HIGHEST);
    SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)_wsv_exception);
    return ERR_OK;
}
static void _wsv_setstatus(DWORD status) {
    svstatus.dwCheckPoint = 0;
    svstatus.dwCurrentState = status;
    SetServiceStatus(psvstatus, &svstatus);
}
static void _wsv_pending(DWORD status, DWORD timeout) {
    svstatus.dwCheckPoint = 0;
    svstatus.dwWaitHint = timeout;
    svstatus.dwCurrentState = status;
    SetServiceStatus(psvstatus, &svstatus);
}
static int32_t _wsv_runfuncs(_wsv_cb *funcs) {
    int32_t index = 0;
    if (funcs) {
        while (NULL != funcs[index]) {
            svstatus.dwCheckPoint++;
            SetServiceStatus(psvstatus, &svstatus);
            if (ERR_OK != (funcs[index])()) {
                return ERR_FAILED;
            }
            index++;
        }
    }
    return ERR_OK;
}
DWORD WINAPI _wsv_event(DWORD req, DWORD event, LPVOID eventdata, LPVOID context) {
    switch (req) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        LOG_INFO("catch sign: %d", req);
        _wsv_pending(SERVICE_STOP_PENDING, WINSV_STOP_TIMEOUT);
        cond_signal(&condexit);
        break;
    default:
        break;
    }
    return ERR_OK;
}
static void WINAPI _wsv_service(DWORD argc, LPTSTR *argv) {
    ZERO(&svstatus, sizeof(svstatus));
    svstatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    svstatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    svstatus.dwCheckPoint = 0;
    char name[PATH_LENS] = { 0 };
    psvstatus = RegisterServiceCtrlHandlerExW((LPWSTR)name, _wsv_event, NULL);
    _wsv_pending(SERVICE_START_PENDING, WINSV_START_TIMEOUT);
    if (ERR_OK == _wsv_runfuncs(initcbs)) {
        _wsv_setstatus(SERVICE_RUNNING);
        mutex_lock(&muexit);
        cond_wait(&condexit, &muexit);
        mutex_unlock(&muexit);
        _wsv_runfuncs(exitcbs);
    }
    _wsv_setstatus(SERVICE_STOPPED);
}
static BOOL wsv_startservice(LPCTSTR name) {
    SERVICE_TABLE_ENTRY st[] = {
        { (LPSTR)name, _wsv_service },
        { NULL, NULL }
    };
    return StartServiceCtrlDispatcher(st);
}
static BOOL wsv_isinstalled(LPCTSTR name) {
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        return TRUE;
    }
    SC_HANDLE service = OpenService(scm, name, SERVICE_QUERY_CONFIG);
    if (!service) {
        CloseServiceHandle(scm);
        return FALSE;
    }
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return TRUE;
}
static _csd_t _wsv_csd(void) {
    HMODULE advapi32;
    if (!(advapi32 = GetModuleHandle("ADVAPI32.DLL"))) {
        return NULL;
    }
    _csd_t csd;
    if (!(csd = (_csd_t)GetProcAddress(advapi32, "ChangeServiceConfig2A"))) {
        return NULL;
    }
    return csd;
}
static BOOL wsv_install(LPCTSTR name) {
    _csd_t csd = _wsv_csd();
    if (NULL == csd) {
        return FALSE;
    }
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        return FALSE;
    }
    char tmp[PATH_LENS] = { 0 };
    char propath[PATH_LENS] = { 0 };
    GetModuleFileName(NULL, propath, sizeof(propath));
    SNPRINTF(tmp, sizeof(tmp) - 1, "\"%s\" \"-r\" \"%s\"", propath, name);
    SC_HANDLE service = CreateService(scm,
                                      name,
                                      name,
                                      SERVICE_ALL_ACCESS,
                                      SERVICE_WIN32_OWN_PROCESS,// | SERVICE_INTERACTIVE_PROCESS(允许服务于桌面交互),
                                      SERVICE_AUTO_START,
                                      SERVICE_ERROR_NORMAL,
                                      tmp,
                                      NULL,
                                      NULL,
                                      NULL,
                                      NULL,
                                      NULL);

    if (!service) {
        CloseServiceHandle(scm);
        return FALSE;
    }
    const char *svdesp = "srey";
    SERVICE_DESCRIPTION desp;
    desp.lpDescription = (LPSTR)svdesp;
    csd(service, SERVICE_CONFIG_DESCRIPTION, &desp);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return TRUE;
}
static BOOL wsv_unInstall(LPCTSTR name) {
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        return FALSE;
    }
    SC_HANDLE service = OpenService(scm, name, DELETE);
    if (!service) {
        CloseServiceHandle(scm);
        return FALSE;
    }
    DeleteService(service);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return TRUE;
}
static void _useage(void) {
    PRINT("UseAge:srey front-end mode;");
    PRINT("srey -i \"service name\" install service;");
    PRINT("srey -u \"service name\" uninstall service;");
    PRINT("srey -r \"service name\" run service.");
}
#else
static void _stop_sh(const char *sh) {
    char cmd[64] = { 0 };
    SNPRINTF(cmd, sizeof(cmd) - 1, "kill -%d %d\n", SIGUSR1, (int32_t)getpid());
    FILE *file = fopen(sh, "w");
    if (NULL == file) {
        return;
    }
    const char *fline = "#!/bin/sh\n";
    fwrite(fline, 1, strlen(fline), file);
    fwrite(cmd, 1, strlen(cmd), file);
    fclose(file);
    chmod(sh, 755);
}
#endif
int main(int argc, char *argv[]) {
#ifdef OS_WIN
    if (1 == argc) {
        return service_hug();
    }
    if (3 != argc) {
        _useage();
        return ERR_FAILED;
    }
    //管理员权限
    if (0 == strcmp("-i", argv[1])) {
        if (wsv_isinstalled(argv[2])) {
            PRINT("service %s exited!", argv[2]);
            return ERR_FAILED;
        }
        if (wsv_install(argv[2])) {
            PRINT("install service %s successfully!", argv[2]);
            return ERR_OK;
        } else {
            PRINT("install service %s error!", argv[2]);
            return ERR_FAILED;
        }
    } else if (0 == strcmp("-u", argv[1])) {
        if (!wsv_isinstalled(argv[2])) {
            PRINT("uninstall service error.service %s not exited!", argv[2]);
            return ERR_FAILED;
        }
        if (wsv_unInstall(argv[2])) {
            PRINT("uninstall service %s successfully!", argv[2]);
            return ERR_OK;
        } else {
            PRINT("uninstall service %s failed!", argv[2]);
            return ERR_FAILED;
        }
    } else if (0 == strcmp("-r", argv[1])) {
        if (wsv_startservice(argv[2])) {
            return ERR_OK;
        } else {
            return ERR_FAILED;
        }
    } else {
        _useage();
        return ERR_FAILED;
    }
#else
    if (argc > 1) {
        if (0 == strcmp ("-d", argv[1])) {
            return service_hug();
        }
        PRINT("UseAge:\"./srey\" or \"./srey -d\".");
        return ERR_FAILED;
    }
    pid_t pid = fork();
    if (0 == pid) {
        char sh[PATH_LENS] = { 0 };
        SNPRINTF(sh, sizeof(sh) - 1, "%s%s%s", procpath(), PATH_SEPARATORSTR, "stop.sh");
        _stop_sh(sh);
        int32_t rtn = service_hug();
        remove(sh);
        return rtn;
    } else if (pid > 0) {
        return ERR_OK;
    } else {
        PRINT("fork process error!");
        return ERR_FAILED;
    }
    return ERR_OK;
#endif
}
