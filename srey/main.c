#include "startup.h"
#include "cjson/cJSON.h"

static FILE *logstream = NULL;
static mutex_ctx muexit;
static cond_ctx condexit;
static char *_config_read(void) {
    char config[PATH_LENS];
    SNPRINTF(config, sizeof(config), "%s%s%s%s%s",
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
static double _json_get_number(cJSON *json, const char *name) {
    cJSON *val = cJSON_GetObjectItem(json, name);
    if (NULL != val
        && cJSON_IsNumber(val)) {
        return val->valuedouble;
    }
    return 0;
}
static void _json_get_string(cJSON *json, const char *name, char *str, size_t lens) {
    cJSON *val = cJSON_GetObjectItem(json, name);
    if (NULL != val
        && cJSON_IsString(val)) {
        size_t vlen = strlen(val->valuestring);
        if (vlen < lens) {
            memcpy(str, val->valuestring, vlen);
            str[vlen] = '\0';
        } else {
            LOG_WARN("%s value too long.", name);
        }
    }
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
    cnf->nnet = (uint16_t)_json_get_number(json, "nnet");
    cnf->nworker = (uint16_t)_json_get_number(json, "nworker");
    cnf->loglv = (uint8_t)_json_get_number(json, "loglv");
    _json_get_string(json, "dns", cnf->dns, sizeof(cnf->dns));
    _json_get_string(json, "script", cnf->script, sizeof(cnf->script));
    cnf->harborname = (name_t)_json_get_number(json, "harborname");
    cnf->harborssl = (name_t)_json_get_number(json, "harborssl");
    cnf->harbortimeout = (int32_t)_json_get_number(json, "harbortimeout");
    _json_get_string(json, "harborip", cnf->harborip, sizeof(cnf->harborip));
    cnf->harborport = (uint16_t)_json_get_number(json, "harborport");
    _json_get_string(json, "harborkey", cnf->harborkey, sizeof(cnf->harborkey));
    cJSON_Delete(json);
}
static void _open_log(void) {
    char logfile[PATH_LENS];
    SNPRINTF(logfile, sizeof(logfile), "%s%s%s%s", procpath(), PATH_SEPARATORSTR, "logs", PATH_SEPARATORSTR);
    if (ERR_OK != ACCESS(logfile, 0)) {
        if (ERR_OK != MKDIR(logfile)) {
            log_init(NULL);
            return;
        }
    }
    size_t lens = strlen(logfile);
    char time[TIME_LENS] = { 0 };
    sectostr(nowsec(), "%Y-%m-%d %H-%M-%S", time);
    SNPRINTF((char*)logfile + lens, sizeof(logfile) - lens, "%s%s", time, ".log");
    logstream = fopen(logfile, "a");
    log_init(logstream);
}
static int32_t service_exit(void) {
    loader_free(g_loader);
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
    config->harborname = 100000,
    config->harbortimeout = -1;
    config->harborport = 8080;
    strcpy(config->harborip, "0.0.0.0");
    strcpy(config->dns, "8.8.8.8");
    ZERO(config->harborkey, sizeof(config->harborkey));
    strcpy(config->script, "script");
}
static int32_t service_init(void) {
    config_ctx config;
    _config_init(&config);
    _parse_config(&config);
    dns_set_ip(config.dns);
    log_setlv((LOG_LEVEL)config.loglv);
    _open_log();
    unlimit();
    mutex_init(&muexit);
    cond_init(&condexit);
    g_loader = loader_init(config.nnet, config.nworker);
    if (ERR_OK != task_startup(g_loader, &config)) {
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
    char tmp[PATH_LENS];
    char propath[PATH_LENS] = { 0 };
    GetModuleFileName(NULL, propath, sizeof(propath));
    SNPRINTF(tmp, sizeof(tmp), "\"%s\" \"-r\" \"%s\"", propath, name);
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
    char cmd[128];
    SNPRINTF(cmd, sizeof(cmd), "kill -%d %d\n", SIGUSR1, (int32_t)getpid());
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
        char sh[PATH_LENS];
        SNPRINTF(sh, sizeof(sh), "%s%s%s", procpath(), PATH_SEPARATORSTR, "stop.sh");
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
