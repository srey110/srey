#include "startup.h"
#include "cjson/cJSON.h"

static FILE *logstream = NULL; // 日志文件流，NULL 表示输出到标准输出
static mutex_ctx muexit;       // 配合退出条件变量使用的互斥锁
static cond_ctx condexit;      // 信号/服务停止时用于唤醒主线程的条件变量

// 读取进程目录下 configs/config.json 的内容，返回堆分配字符串（调用方负责释放）
static char *_config_read(void) {
    char config[PATH_LENS];
    SNPRINTF(config, sizeof(config), "%s%s%s%s%s",
        procpath(), PATH_SEPARATORSTR, "configs", PATH_SEPARATORSTR, "config.json");
    size_t lens;
    char *info = readall(config, &lens);
    if (NULL == info) {
        LOG_WARN("%s", ERRORSTR(errno));
        return NULL;
    }
    return info;
}
// 从 cJSON 对象中读取数值字段，字段不存在或非数字时返回 0
static double _json_get_number(cJSON *json, const char *name) {
    cJSON *val = cJSON_GetObjectItem(json, name);
    if (NULL != val
        && cJSON_IsNumber(val)) {
        return val->valuedouble;
    }
    return 0;
}
// 从 cJSON 对象中读取字符串字段，值过长时打印警告
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
// 解析配置文件，将各字段填充到 config_ctx（解析失败时使用默认值）
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
    cnf->stacksize = (uint32_t)_json_get_number(json, "stacksize");
    _json_get_string(json, "dns", cnf->dns, sizeof(cnf->dns));
    _json_get_string(json, "script", cnf->script, sizeof(cnf->script));
    cnf->harborname = (name_t)_json_get_number(json, "harborname");
    cnf->harborssl = (name_t)_json_get_number(json, "harborssl");
    _json_get_string(json, "harborip", cnf->harborip, sizeof(cnf->harborip));
    cnf->harborport = (uint16_t)_json_get_number(json, "harborport");
    _json_get_string(json, "harborkey", cnf->harborkey, sizeof(cnf->harborkey));
    cJSON_Delete(json);
}
// 在进程目录下创建 logs 目录并打开以当前时间命名的日志文件
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
// 释放所有资源并退出服务（loader、互斥锁、条件变量、日志、socket）
static int32_t service_exit(void) {
    loader_free(g_loader);
    mutex_free(&muexit);
    cond_free(&condexit);
    _memcheck();
    if (NULL != logstream) {
        fclose(logstream);
    }
    sock_clean();
    return ERR_OK;
}
// 初始化配置为内置默认值
static void _config_init(config_ctx *config) {
    ZERO(config, sizeof(config_ctx));
    config->loglv = LOGLV_DEBUG;
    config->harborname = 100000,
        config->harborport = 8080;
    strcpy(config->harborip, "0.0.0.0");
    strcpy(config->dns, "8.8.8.8");
    ZERO(config->harborkey, sizeof(config->harborkey));
    strcpy(config->script, "script");
}
// 初始化全局基础设施（socket、随机数种子、BSON 库）
static void _init_globle(void) {
    sock_init();
    srand((uint32_t)time(NULL));
    bson_globle_init();
}
// 完整初始化服务：加载配置、初始化协程、日志、loader、启动业务任务
static int32_t service_init(void) {
    _init_globle();
    config_ctx config;
    _config_init(&config);
    _parse_config(&config);
    coro_desc_init(config.stacksize);
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
// 信号处理回调：收到退出信号时唤醒主线程
static void _on_sigcb(int32_t sig, void *arg) {
    (void)arg;
    LOG_INFO("catch sign: %d", sig);
    cond_signal(&condexit);
}
// 注册信号处理、启动服务并阻塞等待退出信号
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
#if WITH_LUA
#pragma comment(lib, "lualib.lib")
#endif
#define WINSV_STOP_TIMEOUT       30 * 1000      // Windows 服务停止超时时间（毫秒）
#define WINSV_START_TIMEOUT      30 * 1000      // Windows 服务启动超时时间（毫秒）

typedef WINADVAPI BOOL(WINAPI *_csd_t)(SC_HANDLE, DWORD, LPCVOID); // ChangeServiceConfig2A 函数指针类型
typedef int32_t(*_wsv_cb)(void); // Windows 服务初始化/退出回调函数类型
static int32_t _wsv_initbasic(void);
static _wsv_cb initcbs[] = { _wsv_initbasic, service_init, NULL };
static _wsv_cb exitcbs[] = { service_exit, NULL };
static SERVICE_STATUS_HANDLE psvstatus;
static SERVICE_STATUS svstatus;

// 全局异常过滤器：捕获未处理异常，避免系统弹出崩溃对话框
static long _wsv_exception(struct _EXCEPTION_POINTERS *exp) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    return EXCEPTION_EXECUTE_HANDLER;
}
// 设置进程/线程优先级为最高，并注册全局异常过滤器
static int32_t _wsv_initbasic(void) {
    HANDLE curproc = GetCurrentProcess();
    SetPriorityClass(curproc, HIGH_PRIORITY_CLASS);
    SetThreadPriority(curproc, THREAD_PRIORITY_HIGHEST);
    SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER)_wsv_exception);
    return ERR_OK;
}
// 向 SCM 上报服务当前状态
static void _wsv_setstatus(DWORD status) {
    svstatus.dwCheckPoint = 0;
    svstatus.dwCurrentState = status;
    SetServiceStatus(psvstatus, &svstatus);
}
// 向 SCM 上报服务挂起状态（启动中/停止中）及等待超时提示
static void _wsv_pending(DWORD status, DWORD timeout) {
    svstatus.dwCheckPoint = 0;
    svstatus.dwWaitHint = timeout;
    svstatus.dwCurrentState = status;
    SetServiceStatus(psvstatus, &svstatus);
}
// 依次执行回调函数列表，任一失败立即返回 ERR_FAILED
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
// Windows 服务控制事件处理函数（停止/关机时唤醒主线程）
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
// Windows 服务主函数：注册控制处理器，执行初始化，阻塞等待停止信号
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
// 以服务模式启动并连接到 SCM
static BOOL wsv_startservice(LPCTSTR name) {
    SERVICE_TABLE_ENTRY st[] = {
        { (LPSTR)name, _wsv_service },
        { NULL, NULL }
    };
    return StartServiceCtrlDispatcher(st);
}
// 检查指定名称的 Windows 服务是否已安装
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
// 动态加载 ADVAPI32.DLL 并获取 ChangeServiceConfig2A 函数指针
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
// 安装 Windows 服务（自动启动，设置服务描述为 "srey"）
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
// 卸载指定名称的 Windows 服务
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
// 打印 Windows 下的命令行使用说明
static void _useage(void) {
    PRINT("UseAge:srey front-end mode;");
    PRINT("srey -i \"service name\" install service;");
    PRINT("srey -u \"service name\" uninstall service;");
    PRINT("srey -r \"service name\" run service.");
}
#else
// 在进程目录下生成 stop.sh 脚本，内容为向自身发送 SIGUSR1 信号
static void _stop_sh(const char *sh) {
    char cmd[128];
    SNPRINTF(cmd, sizeof(cmd), "kill -%d %d\n", SIGUSR1, (int32_t)GETPID());
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
    // 以下操作需要管理员权限
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
        if (0 == strcmp("-d", argv[1])) {
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
