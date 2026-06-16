#include "startup.h"
#include "cjson/cJSON.h"
#if WITH_LUA && ENABLE_LUA_BYTECACHE
#include "lbind/lbytecache.h"
#endif

static int32_t _log_use_file = 1; //是否将日志写文件
static FILE *logstream = NULL; // 日志文件流，NULL 表示输出到标准输出
static hug_ctx _hug;           // 退出等待原语 (信号 handler 通过 sighandle data 拿到 &_hug 调 hug_wakeup)

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
// 从 cJSON 对象中读取数值字段，字段不存在、非数字、或不在 [0, max] 范围内时返回 dft
static double _json_get_number(cJSON *json, const char *name, double dft, double max) {
    cJSON *val = cJSON_GetObjectItem(json, name);
    if (NULL != val
        && cJSON_IsNumber(val)) {
        double d = val->valuedouble;
        if (isnan(d) || isinf(d) || d < 0 || d > max) {
            LOG_WARN("%s is NaN/Inf/out of range, use default.", name);
            return dft;
        }
        return d;
    }
    return dft;
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
    cnf->serviceid = (uint16_t)_json_get_number(json, "serviceid", cnf->serviceid, UINT16_MAX);
    cnf->nnet = (uint16_t)_json_get_number(json, "nnet", cnf->nnet, UINT16_MAX);
    cnf->nworker = (uint16_t)_json_get_number(json, "nworker", cnf->nworker, UINT16_MAX);
    cnf->loglv = (uint8_t)_json_get_number(json, "loglv", cnf->loglv, UINT8_MAX);
    cnf->stacksize = (uint32_t)_json_get_number(json, "stacksize", cnf->stacksize, UINT32_MAX);
    cnf->twqueuelens = (uint32_t)_json_get_number(json, "twqueuelens", cnf->twqueuelens, UINT32_MAX);
    cnf->logqueuelens = (uint32_t)_json_get_number(json, "logqueuelens", cnf->logqueuelens, UINT32_MAX);
    _json_get_string(json, "dns", cnf->dns, sizeof(cnf->dns));
    _json_get_string(json, "script", cnf->script, sizeof(cnf->script));
    // debug / harbor / datacenter / subcenter 各为嵌套对象
    cJSON *debug = cJSON_GetObjectItem(json, "debug");
    if (NULL != debug) {
        _json_get_string(debug, "name", cnf->debug.name, sizeof(cnf->debug.name));
        _json_get_string(debug, "ip", cnf->debug.ip, sizeof(cnf->debug.ip));
        cnf->debug.port = (uint16_t)_json_get_number(debug, "port", cnf->debug.port, UINT16_MAX);
    }
    cJSON *harbor = cJSON_GetObjectItem(json, "harbor");
    if (NULL != harbor) {
        _json_get_string(harbor, "name", cnf->harbor.name, sizeof(cnf->harbor.name));
        _json_get_string(harbor, "ssl", cnf->harbor.ssl, sizeof(cnf->harbor.ssl));
        _json_get_string(harbor, "ip", cnf->harbor.ip, sizeof(cnf->harbor.ip));
        cnf->harbor.port = (uint16_t)_json_get_number(harbor, "port", cnf->harbor.port, UINT16_MAX);
        _json_get_string(harbor, "key", cnf->harbor.key, sizeof(cnf->harbor.key));
    }
    cJSON *datacenter = cJSON_GetObjectItem(json, "datacenter");
    if (NULL != datacenter) {
        _json_get_string(datacenter, "name", cnf->datacenter.name, sizeof(cnf->datacenter.name));
    }
    cJSON *subcenter = cJSON_GetObjectItem(json, "subcenter");
    if (NULL != subcenter) {
        _json_get_string(subcenter, "name", cnf->subcenter.name, sizeof(cnf->subcenter.name));
        _json_get_string(subcenter, "rule", cnf->subcenter.rule, sizeof(cnf->subcenter.rule));
    }
    cJSON_Delete(json);
}
// 在进程目录下创建 logs 目录并打开以当前时间命名的日志文件
static void _open_log(uint32_t capacity) {
    if (!_log_use_file) {
        log_init(NULL, capacity);
        return;
    }
    char logfile[PATH_LENS];
    SNPRINTF(logfile, sizeof(logfile), "%s%s%s%s", procpath(), PATH_SEPARATORSTR, "logs", PATH_SEPARATORSTR);
    if (ERR_OK != ACCESS(logfile, 0)) {
        if (ERR_OK != MKDIR(logfile)) {
            log_init(NULL, capacity);
            return;
        }
    }
    size_t lens = strlen(logfile);
    char time[TIME_LENS] = { 0 };
    if (ERR_OK != sectostr(nowsec(), "%Y-%m-%d", time)) {
        // sectostr 失败 fallback：用 pid + 当前毫秒，避免多次启动共享 .log 文件名
        SNPRINTF(time, sizeof(time), "%d_%"PRIu64, (int32_t)GETPID(), nowms());
    }
    SNPRINTF((char*)logfile + lens, sizeof(logfile) - lens, "%s%s", time, ".log");
    logstream = fopen(logfile, "a");
    if (NULL == logstream) {
        // fopen 失败时退化为终端输出；写 stderr 以便部署排查
        fprintf(stderr, "open log file %s failed: %s\n", logfile, ERRORSTR(errno));
    } else {
#ifndef OS_WIN
        PRINT("tail -f \"%s\"", logfile);
#endif
    }
    log_init(logstream, capacity);
}
// 释放所有资源并退出服务（loader、日志、socket）
static int32_t service_exit(void) {
    loader_free(g_loader);
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    lbc_free();
#endif
    log_free();
    if (NULL != logstream) {
        fclose(logstream);
        logstream = NULL;
    }
    sock_clean();
#if defined(OS_WIN)
    timeEndPeriod(1);
#endif
    _memcheck();
    return ERR_OK;
}
// 初始化配置为内置默认值
static void _config_init(config_ctx *config) {
    ZERO(config, sizeof(config_ctx));
    config->serviceid = 1;
    config->loglv = LOGLV_DEBUG;
    config->harbor.port = 0;
    config->debug.port = 0;     // 端口 0 关闭 debug_console,可由 config.json "debug.port" 覆盖
    SNPRINTF(config->harbor.name, sizeof(config->harbor.name), "%s", "harbor");
    SNPRINTF(config->datacenter.name, sizeof(config->datacenter.name), "%s", "datacenter"); // 空串关闭 DataCenter,可由 config.json "datacenter.name" 覆盖
    SNPRINTF(config->subcenter.name, sizeof(config->subcenter.name), "%s", "subcenter");  // 空串关闭 subcenter,可由 config.json "subcenter.name" 覆盖
    SNPRINTF(config->subcenter.rule, sizeof(config->subcenter.rule), "%s", "def");        // subcenter 默认通用 pub/sub 规则
    SNPRINTF(config->debug.name, sizeof(config->debug.name), "%s", "debug");
    SNPRINTF(config->harbor.ip, sizeof(config->harbor.ip), "%s", "0.0.0.0");
    SNPRINTF(config->debug.ip, sizeof(config->debug.ip), "%s", "127.0.0.1");
    SNPRINTF(config->dns, sizeof(config->dns), "%s", "8.8.8.8");
    SNPRINTF(config->script, sizeof(config->script), "%s", "script");
}
// 初始化全局基础设施（socket、随机数种子、BSON 库）
static void _init_globle(void) {
#if defined(OS_WIN)
    // 提升 Windows 系统定时器精度至 1ms，使 cond_timedwait 等睡眠接口得到更准确的唤醒
    timeBeginPeriod(1);
#endif
    sock_init();
    srand((uint32_t)(time(NULL) ^ nowms() ^ GETPID()));
    bson_globle_init();
}
// 完整初始化服务：加载配置、初始化协程、日志、loader、启动业务任务
static int32_t service_init(void) {
    _init_globle();
    config_ctx config;
    _config_init(&config);
    _parse_config(&config);
    if (ERR_OK != serviceid(config.serviceid)) {
        secure_zero(config.harbor.key, sizeof(config.harbor.key));
        PRINT("serviceid error.");
        return ERR_FAILED;
    }
    coro_desc_init(config.stacksize);
    dns_set_ip(config.dns);
    log_setlv((LOG_LEVEL)config.loglv);
    _open_log(config.logqueuelens);
    unlimit();
    g_loader = loader_init(config.nnet, config.nworker, config.twqueuelens);
#if WITH_LUA && ENABLE_LUA_BYTECACHE
    lbc_init(loader_lckcache(g_loader));
#endif
    if (ERR_OK != task_startup(g_loader, &config)) {
        secure_zero(config.harbor.key, sizeof(config.harbor.key));
        service_exit();
        return ERR_FAILED;
    }
    secure_zero(config.harbor.key, sizeof(config.harbor.key));
    return ERR_OK;
}
// 信号处理回调: 通过 sighandle data 拿到 hug_ctx, 转发到 hug_wakeup 唤醒主线程
static void _on_sigcb(int32_t sig, void *arg) {
    (void)sig;
    hug_wakeup((hug_ctx *)arg);
}
// 注册信号处理、启动服务并阻塞等待退出信号
// ready_fd：daemon 化父子同步 pipe 写端，-1 表示无需通知（Windows / -d 前台模式）；
// service_init 成功时写 'R' 通知父进程；失败时仅 close 让父进程 read 返 0 (EOF) 即知失败
static int32_t service_hug(int32_t ready_fd) {
    if (ERR_OK != hug_init(&_hug)) {
#ifndef OS_WIN
        if (ready_fd >= 0) {
            close(ready_fd);
        }
#else
    (void)ready_fd;//Windows 永远传 -1, 不需要 daemon 父子同步
#endif
        return ERR_FAILED;
    }
    sighandle(_on_sigcb, &_hug);
    int32_t rtn = service_init();
#ifndef OS_WIN
    if (ready_fd >= 0) {
        if (ERR_OK == rtn) {
            char r = 'R';
            (void)!write(ready_fd, &r, 1);
        }
        close(ready_fd);
    }
#endif
    if (ERR_OK == rtn) {
        hug_wait(&_hug);
        service_exit();
    }
    hug_free(&_hug);
    return rtn;
}
#ifdef OS_WIN
    //#include "vld.h"
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "winmm.lib")
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
#define WINSV_STOP_TIMEOUT       (30 * 1000)    // Windows 服务停止超时时间（毫秒）
#define WINSV_START_TIMEOUT      (30 * 1000)    // Windows 服务启动超时时间（毫秒）

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
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
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
// SCM 线程上下文不是真 signal handler, 直接走 hug_wakeup 即可
DWORD WINAPI _wsv_event(DWORD req, DWORD event, LPVOID eventdata, LPVOID context) {
    switch (req) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        LOG_INFO("catch sign: %d", req);
        _wsv_pending(SERVICE_STOP_PENDING, WINSV_STOP_TIMEOUT);
        hug_wakeup(&_hug);
        break;
    default:
        break;
    }
    return ERR_OK;
}
// Windows 服务主函数：注册控制处理器，执行初始化，阻塞等待停止信号
static void WINAPI _wsv_service(DWORD argc, LPTSTR *argv) {
    (void)argc;
    ZERO(&svstatus, sizeof(svstatus));
    svstatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    svstatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    svstatus.dwCheckPoint = 0;
    if (ERR_OK != hug_init(&_hug)) {
        return;
    }
    psvstatus = RegisterServiceCtrlHandlerExA(argv[0], _wsv_event, NULL);
    _wsv_pending(SERVICE_START_PENDING, WINSV_START_TIMEOUT);
    if (ERR_OK == _wsv_runfuncs(initcbs)) {
        _wsv_setstatus(SERVICE_RUNNING);
        hug_wait(&_hug);
        _wsv_runfuncs(exitcbs);
    }
    _wsv_setstatus(SERVICE_STOPPED);
    hug_free(&_hug);
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
    chmod(sh, 0755);
}
#endif
int main(int argc, char *argv[]) {
#ifdef OS_WIN
    if (1 == argc) {
        _log_use_file = 0;
        return service_hug(-1);
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
    if (argc > 1 && 0 == strcmp("-d", argv[1])) {
        _log_use_file = 0;
        return service_hug(-1);
    }
    if (argc > 1 && 0 != strcmp("-b", argv[1])) {
        PRINT("UseAge:\"./srey\" or \"./srey -d\" or \"./srey -b\".");
        return ERR_FAILED;
    }
    int32_t is_daemon = (argc > 1 && 0 == strcmp("-b", argv[1]));
    // daemon 模式下用 pipe 同步：子进程 service_init 成功写 'R'，失败 close 让父端读 EOF。
    // 防止父进程在子进程 daemon 化（setsid/chdir/open）或 service_init 失败时报假成功
    int32_t sync_pipe[2] = { -1, -1 };
    if (is_daemon) {
        if (-1 == pipe(sync_pipe)) {
            PRINT("pipe error: %s", ERRORSTR(errno));
            return ERR_FAILED;
        }
    }
    pid_t pid = fork();
    if (0 == pid) {
        // 子进程：关 pipe 读端，daemon 化途中任一失败先 close 写端再 exit，让父端 read EOF
        if (is_daemon) {
            close(sync_pipe[0]);
            //daemon 化：脱离控制终端，重定向标准 IO 到 /dev/null
            if ((pid_t)-1 == setsid()) {
                PRINT("setsid error: %s", ERRORSTR(errno));
                close(sync_pipe[1]);
                return ERR_FAILED;
            }
            //切换到 procpath()（已是绝对路径），避免工作目录被 unmount 时进程卡住
            if (0 != chdir(procpath())) {
                PRINT("chdir error: %s", ERRORSTR(errno));
                close(sync_pipe[1]);
                return ERR_FAILED;
            }
            //关闭并重定向 stdin/stdout/stderr 到 /dev/null
            int32_t devnull = open("/dev/null", O_RDWR);
            if (-1 == devnull) {
                PRINT("open /dev/null error: %s", ERRORSTR(errno));
                close(sync_pipe[1]);
                return ERR_FAILED;
            }
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        char sh[PATH_LENS];
        SNPRINTF(sh, sizeof(sh), "%s%s%s", procpath(), PATH_SEPARATORSTR, "stop.sh");
        _stop_sh(sh);
        int32_t rtn = service_hug(is_daemon ? sync_pipe[1] : -1);
        remove(sh);//服务退出，移除stop.sh
        return rtn;
    } else if (pid > 0) {
        if (is_daemon) {
            close(sync_pipe[1]);
            char r = 0;
            ssize_t n;
            //EINTR 重试；read 返 1 + 'R' = daemon + service_init 成功；返 0 (EOF) = 子进程失败
            do {
                n = read(sync_pipe[0], &r, 1);
            } while (-1 == n && EINTR == errno);
            close(sync_pipe[0]);
            if (1 != n || 'R' != r) {
                int wstatus;
                pid_t r2;
                do {
                    r2 = waitpid(pid, &wstatus, 0);
                } while (-1 == r2 && EINTR == errno);
                if (pid == r2) {
                    if (WIFEXITED(wstatus)) {
                        PRINT("daemon child exited with code %d", WEXITSTATUS(wstatus));
                    } else if (WIFSIGNALED(wstatus)) {
                        PRINT("daemon child killed by signal %d", WTERMSIG(wstatus));
                    }
                }
                return ERR_FAILED;
            }
        }
        return ERR_OK;
    } else {
        PRINT("fork process error!");
        if (is_daemon) {
            close(sync_pipe[0]);
            close(sync_pipe[1]);
        }
        return ERR_FAILED;
    }
#endif
}
