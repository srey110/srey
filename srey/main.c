#include "startup.h"

#ifdef OS_WIN
    //#include "vld.h"
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "winmm.lib")
    #pragma comment(lib, "lib.lib")
    #if WITH_MIMALLOC
        #pragma comment(lib, "mimalloc" DEPS_LIB_SUFFIX ".lib")
    #endif
    // 库名后缀由 os.h 的 DEPS_LIB_SUFFIX 拼,对应 tools/deps.py 落在 bin/ 的那个变体。
    // 后四个是静态 OpenSSL 自己声明的 Windows 依赖(见其 Configurations/10-main.conf 的
    // ex_libs),链动态库时由 DLL 自带,链静态库就得调用方补上
    #if WITH_SSL
        #pragma comment(lib, "libcrypto" DEPS_LIB_SUFFIX ".lib")
        #pragma comment(lib, "libssl" DEPS_LIB_SUFFIX ".lib")
        #pragma comment(lib, "crypt32.lib")
        #pragma comment(lib, "advapi32.lib")
        #pragma comment(lib, "user32.lib")
        #pragma comment(lib, "gdi32.lib")
    #endif
    #if WITH_LUA
        #pragma comment(lib, "lualib.lib")
    #endif
    // 下面两个 typedef 用到的 SC_HANDLE / WINADVAPI 出自 winsvc.h, 由 os.h 的 <Windows.h> 带入
    #define WINSV_STOP_TIMEOUT (30 * 1000) // Windows 服务停止超时时间（毫秒）
    #define WINSV_START_TIMEOUT (30 * 1000) // Windows 服务启动超时时间（毫秒）
    #define WINSV_INIT_FAILED 1 // service_init 失败报给 SCM 的 service-specific 码；具体原因只在日志里
    typedef WINADVAPI BOOL(WINAPI *_csd_t)(SC_HANDLE, DWORD, LPCVOID); // ChangeServiceConfig2A 函数指针类型
    typedef int32_t(*_wsv_cb)(void); // Windows 服务初始化/退出回调函数类型

    // 三个都定义在本文件下方; 两张回调表挪到文件头后必须先声明, 否则是引用未声明标识符
    static int32_t _wsv_initbasic(void);
    static int32_t service_init(void);
    static int32_t service_exit(void);
    static _wsv_cb initcbs[] = { _wsv_initbasic, service_init, NULL };
    static _wsv_cb exitcbs[] = { service_exit, NULL };
    static SERVICE_STATUS_HANDLE psvstatus;
    static SERVICE_STATUS svstatus;
#endif//OS_WIN
static int32_t _log_use_file = 1; //是否将日志写文件
static FILE *logstream = NULL; // 日志文件流，NULL 表示输出到标准输出
static hug_ctx _hug; // 退出等待原语 (信号 handler 通过 sighandle data 拿到 &_hug 调 hug_wakeup)

// 读一个配置字段：取到就写进去，取不到且字段确实存在才告警（可选字段缺席是正常的）。
// 键名在整条语句里只出现一次：取值与告警共用同一个 keystr，不会读一个键报另一个键
// it 是该对象的 yyjson_obj_iter：从上次命中处往后找，到末尾绕回开头，按文件键序查询时每次只比一个键
#define CFG_NUM(it, prefix, keystr, max, field, type) do { \
        double _v; \
        yyjson_val *_jv = yyjson_obj_iter_get((it), (keystr)); \
        if (ERR_OK == json_val_num_range(_jv, 0, (max), &_v)) { \
            (field) = (type)_v; \
        } else if (NULL != _jv) { \
            PRINT("%s%s invalid, use default.", (prefix), (keystr)); \
        } \
    } while (0)
#define CFG_STR(it, prefix, keystr, field) do { \
        yyjson_val *_jv = yyjson_obj_iter_get((it), (keystr)); \
        if (ERR_OK != json_val_string(_jv, (field), sizeof(field)) \
            && NULL != _jv) { \
            PRINT("%s%s invalid, use default.", (prefix), (keystr)); \
        } \
    } while (0)
// 拼路径后判有没有截断。SNPRINTF 截断时返回的是"本应写入的长度"而不是实际写入，
// 不判就会拿到一条指向别处的路径 —— 建目录建到别处、删文件删到别处
static int32_t _path_ok(int32_t rtn, size_t cap) {
    return rtn >= 0 && (size_t)rtn < cap;
}
// 读取进程目录下 configs/config.json 的内容，返回堆分配字符串（调用方负责释放）。
// lens 交出 readall 记的真实字节数：不能让调用方拿 strlen 重推——文件里出现 NUL 时
// strlen 短于真实长度，截断后恰好合法的 JSON 前缀会被静默当成整份配置收下
static char *_config_read(size_t *lens) {
    char config[PATH_LENS];
    int32_t plen = SNPRINTF(config, sizeof(config), "%s%s%s%s%s",
        procpath(), PATH_SEPARATORSTR, "configs", PATH_SEPARATORSTR, "config.json");
    if (!_path_ok(plen, sizeof(config))) {
        PRINT("config path too long under %s.", procpath());
        return NULL;
    }
    char *info = readall(config, lens);
    if (NULL == info) {
        PRINT("%s", strerror(errno));
        return NULL;
    }
    return info;
}
// 解析配置文件，将各字段填充到 config_ctx（解析失败时使用默认值）
static void _parse_config(config_ctx *cnf) {
    size_t lens;
    char *config = _config_read(&lens);
    if (NULL == config) {
        return;
    }
    yyjson_read_err erro;
    yyjson_doc *doc = yyjson_read_opts(config, lens, YYJSON_READ_ALLOW_BOM, NULL, &erro);
    FREE(config);
    if (NULL == doc) {
        PRINT("parse config error at byte %zu: %s", erro.pos, erro.msg);
        return;
    }
    yyjson_obj_iter it;
    yyjson_obj_iter sub;
    if (!yyjson_obj_iter_init(yyjson_doc_get_root(doc), &it)) {
        yyjson_doc_free(doc);
        return;
    }
    CFG_NUM(&it, "", "serviceid", SERVICEID_MAX, cnf->serviceid, uint16_t);
    CFG_NUM(&it, "", "nnet", UINT16_MAX, cnf->nnet, uint16_t);
    CFG_NUM(&it, "", "nworker", UINT16_MAX, cnf->nworker, uint16_t);
    CFG_NUM(&it, "", "loglv", LOGLV_DEBUG, cnf->loglv, uint8_t);
    CFG_NUM(&it, "", "stacksize", UINT32_MAX, cnf->stacksize, uint32_t);
    CFG_NUM(&it, "", "twqueuelens", UINT32_MAX, cnf->twqueuelens, uint32_t);
    CFG_NUM(&it, "", "logqueuelens", UINT32_MAX, cnf->logqueuelens, uint32_t);
    CFG_STR(&it, "", "dns", cnf->dns);
    CFG_STR(&it, "", "script", cnf->script);
    // debug / harbor 各为嵌套对象
    if (yyjson_obj_iter_init(yyjson_obj_iter_get(&it, "debug"), &sub)) {
        CFG_STR(&sub, "debug.", "name", cnf->debug.name);
        CFG_STR(&sub, "debug.", "ip", cnf->debug.ip);
        CFG_NUM(&sub, "debug.", "port", UINT16_MAX, cnf->debug.port, uint16_t);
    }
    if (yyjson_obj_iter_init(yyjson_obj_iter_get(&it, "harbor"), &sub)) {
        CFG_STR(&sub, "harbor.", "name", cnf->harbor.name);
        CFG_STR(&sub, "harbor.", "ssl", cnf->harbor.ssl);
        CFG_STR(&sub, "harbor.", "ip", cnf->harbor.ip);
        CFG_NUM(&sub, "harbor.", "port", UINT16_MAX, cnf->harbor.port, uint16_t);
    }
    yyjson_doc_free(doc);
}
// 在进程目录下创建 logs 目录并打开以当前时间命名的日志文件。
// 文件缓冲开到 64KB(须在首次读写前设)
static void _open_log(uint32_t capacity) {
    if (!_log_use_file) {
        log_init(NULL, capacity);
        return;
    }
    char logfile[PATH_LENS];
    int32_t plen = SNPRINTF(logfile, sizeof(logfile), "%s%s%s%s",
        procpath(), PATH_SEPARATORSTR, "logs", PATH_SEPARATORSTR);
    if (!_path_ok(plen, sizeof(logfile))) {
        // 装不下就退化成终端输出：截断后的路径指向别的目录，建出来的 logs 与写进去的文件都不在预期位置
        fprintf(stderr, "log dir path too long, log to terminal.\n");
        log_init(NULL, capacity);
        return;
    }
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
    plen = SNPRINTF((char*)logfile + lens, sizeof(logfile) - lens, "%s%s", time, ".log");
    if (!_path_ok(plen, sizeof(logfile) - lens)) {
        fprintf(stderr, "log file path too long, log to terminal.\n");
        log_init(NULL, capacity);
        return;
    }
    logstream = fopen_cloexec(logfile, "a");
    if (NULL == logstream) {
        // fopen 失败时退化为终端输出；写 stderr 以便部署排查
        fprintf(stderr, "open log file %s failed: %s\n", logfile, strerror(errno));
    } else {
        setvbuf(logstream, NULL, _IOFBF, 64 * ONEK);
#ifndef OS_WIN
        PRINT("tail -f \"%s\"", logfile);
#endif
    }
    log_init(logstream, capacity);
}
// thread_global_hooks 的全局 exit:与具体模块无关的线程级缓存在这里收,与 test/main.c 同步。
// Windows 上静态链接的 OpenSSL 不随线程退出释放线程级状态，每条线程须自己调 OPENSSL_thread_stop
// (主线程由 OPENSSL_cleanup 收，-r 服务模式下跑 service_init/service_exit 的服务线程由 service_exit 收)
static void _thread_exit_hook(void *udata, void *assist) {
    (void)udata;
    (void)assist;
    buffer_thread_cleanup();
#if WITH_SSL && defined(OS_WIN)
    OPENSSL_thread_stop();
#endif
}
// 初始化全局基础设施（线程全局钩子、socket、随机数种子、BSON 库）。线程钩子须早于任何线程创建
static void _init_globle(void) {
    thread_global_hooks(NULL, _thread_exit_hook);
#if defined(OS_WIN)
    // 提升 Windows 系统定时器精度至 1ms，使 cond_timedwait 等睡眠接口得到更准确的唤醒
    timeBeginPeriod(1);
#endif
    locale_init();
    sock_init();
    srand((uint32_t)(time(NULL) ^ nowms() ^ GETPID()));
    bson_globle_init();
}
// 与 _init_globle 成对
static void _free_globle(void) {
    locale_free();
    sock_clean();
#if defined(OS_WIN)
    timeEndPeriod(1);
#endif
}
// 释放所有资源并退出服务（loader、日志、socket）
static int32_t service_exit(void) {
    loader_free(g_loader);
    task_cleanup();
    log_free();
    if (NULL != logstream) {
        fclose(logstream);
        logstream = NULL;
    }
    _free_globle();
    buffer_thread_cleanup();
#if WITH_SSL && defined(OS_WIN)
    // -r 服务模式下本函数跑在 SCM 建的服务线程上，不经 thread_creat，全局 exit 钩子管不到：这里收它的
    // OpenSSL 线程级状态。放在最后，之后再碰 OpenSSL 会重新分配；前台模式下是主线程，提前收一次也无害
    OPENSSL_thread_stop();
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
    config->debug.port = 0; // 端口 0 关闭 debug_console,可由 config.json "debug.port" 覆盖
    safe_fill_str(config->harbor.name, sizeof(config->harbor.name), "harbor");
    safe_fill_str(config->debug.name, sizeof(config->debug.name), "debug");
    safe_fill_str(config->harbor.ip, sizeof(config->harbor.ip), "0.0.0.0");
    safe_fill_str(config->debug.ip, sizeof(config->debug.ip), "127.0.0.1");
    safe_fill_str(config->dns, sizeof(config->dns), "8.8.8.8");
    safe_fill_str(config->script, sizeof(config->script), "script");
}
// 完整初始化服务：加载配置、初始化协程、日志、loader、启动业务任务。
// test/main.c 的 main 另有一套等价的全局初始化，加减项要两处同步
static int32_t service_init(void) {
    _init_globle();
    config_ctx config;
    _config_init(&config);
    _parse_config(&config);
    if (ERR_OK != serviceid(config.serviceid)) {
        PRINT("serviceid error.");
        // 这条早退在 _open_log 之前,不能借 service_exit:它无条件 log_free,会去锁没初始化的 mutex
        _free_globle();
        return ERR_FAILED;
    }
    dns_set_ip(config.dns);
    log_setlv((log_level)config.loglv);
    _open_log(config.logqueuelens);
    // 须排在 _open_log 之后:stacksize 越界时它会打一行 WARN,更早打的话 daemon 模式下进不了日志
    coro_task_stack(config.stacksize);
    unlimit();
    g_loader = loader_init(config.nnet, config.nworker, config.twqueuelens);
    if (ERR_OK != task_startup(g_loader, &config)) {
        service_exit();
        return ERR_FAILED;
    }
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
// devnull >= 0 时把标准 IO 接到它上面，但只在 service_init 成功之后——配置诊断全走 PRINT
// 且排在 _open_log 之前，提前接过去的话配置错的 daemon 既不打终端也不进日志文件地静默起来
static int32_t service_hug(int32_t ready_fd, int32_t devnull) {
    if (ERR_OK != hug_init(&_hug)) {
#ifndef OS_WIN
        if (ready_fd >= 0) {
            close(ready_fd);
        }
        if (devnull >= 0) {
            close(devnull);
        }
#else
    (void)ready_fd;//Windows 永远传 -1, 不需要 daemon 父子同步
    (void)devnull;
#endif
        return ERR_FAILED;
    }
    sighandle(_on_sigcb, &_hug);
    int32_t rtn = service_init();
#ifndef OS_WIN
    if (devnull >= 0) {
        if (ERR_OK == rtn) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        if (devnull > STDERR_FILENO) {
            close(devnull);
        }
    }
    if (ready_fd >= 0) {
        if (ERR_OK == rtn) {
            char r = 'R';
            (void)!write(ready_fd, &r, 1);
        }
        close(ready_fd);
    }
#else
    (void)devnull;
#endif
    if (ERR_OK == rtn) {
        hug_wait(&_hug);
        service_exit();
    }
    hug_free(&_hug);
    return rtn;
}
#ifdef OS_WIN

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
// 向 SCM 上报服务的终态(RUNNING / STOPPED)。win32err 非 NO_ERROR 即异常停止。
// dwWaitHint 一并清零:它只对挂起状态有意义,挂起态走 _wsv_pending
static void _wsv_setstatus(DWORD status, DWORD win32err) {
    svstatus.dwCheckPoint = 0;
    svstatus.dwWaitHint = 0;
    svstatus.dwWin32ExitCode = win32err;
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
    DWORD win32err = ERROR_SERVICE_SPECIFIC_ERROR;
    svstatus.dwServiceSpecificExitCode = WINSV_INIT_FAILED;
    _wsv_pending(SERVICE_START_PENDING, WINSV_START_TIMEOUT);
    if (ERR_OK == _wsv_runfuncs(initcbs)) {
        _wsv_setstatus(SERVICE_RUNNING, NO_ERROR);
        hug_wait(&_hug);
        _wsv_runfuncs(exitcbs);
        win32err = NO_ERROR;
    }
    _wsv_setstatus(SERVICE_STOPPED, win32err);
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
    // ImagePath = "<自身路径>" "-r" "<服务名>"：propath 最长 PATH_LENS-1，服务名 Windows 上限
    // 256，加固定的引号与 "-r" 共 10 字节；+512 把这两截连同 NUL 一起兜住，合法入参不会截断
    char tmp[PATH_LENS + 512];
    char propath[PATH_LENS] = { 0 };
    // 两个返回值都必须查：GetModuleFileName 装不下时返回缓冲大小(XP 还不补 NUL)，
    // snprintf 截断时返回"本应写入的长度"。任一漏查都会把残缺 ImagePath 交给 CreateService，
    // 而它并不校验，注册照样成功、这里照打"install successfully"，SCM 之后永远起不来该服务
    DWORD plen = GetModuleFileName(NULL, propath, (DWORD)sizeof(propath));
    if (0 == plen
        || plen >= sizeof(propath)) {
        PRINT("get module file name failed or path too long.");
        CloseServiceHandle(scm);
        return FALSE;
    }
    int32_t tlen = SNPRINTF(tmp, sizeof(tmp), "\"%s\" \"-r\" \"%s\"", propath, name);
    if (!_path_ok(tlen, sizeof(tmp))) {
        PRINT("service image path too long, install aborted.");
        CloseServiceHandle(scm);
        return FALSE;
    }
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
    FILE *file = fopen_cloexec(sh, "w");
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
        return service_hug(-1, -1);
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
        return service_hug(-1, -1);
    }
    if (argc > 1 && 0 != strcmp("-b", argv[1])) {
        PRINT("UseAge:\"./srey\" or \"./srey -d\" or \"./srey -b\".");
        return ERR_FAILED;
    }
    int32_t is_daemon = (argc > 1);
    // 无参与 -b 都要 fork，两条都用 pipe 同步：子进程 service_init 成功写 'R'，失败 close
    // 让父端读 EOF。少了它父进程只能恒返 ERR_OK，配置错、端口占用一律报成功，
    // `./srey && echo ok` 在服务根本没起来时照样打 ok
    int32_t sync_pipe[2] = { -1, -1 };
    if (-1 == pipe(sync_pipe)) {
        PRINT("pipe error: %s", ERRORSTR(errno));
        return ERR_FAILED;
    }
    pid_t pid = fork();
    if (0 == pid) {
        // 子进程：关 pipe 读端，daemon 化途中任一失败先 close 写端再 exit，让父端 read EOF
        close(sync_pipe[0]);
        int32_t devnull = -1;
        if (is_daemon) {
            //daemon 化：脱离控制终端，标准 IO 稍后由 service_hug 接到 /dev/null
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
            devnull = open("/dev/null", O_RDWR);
            if (-1 == devnull) {
                PRINT("open /dev/null error: %s", ERRORSTR(errno));
                close(sync_pipe[1]);
                return ERR_FAILED;
            }
        }
        char sh[PATH_LENS];
        // 截断后 sh 指向别的路径：_stop_sh 会往那儿写，退出时 remove 又会删那儿。
        // 装不下就整个跳过——不给这个便利脚本，也好过写错地方再删错地方
        int32_t has_sh = _path_ok(SNPRINTF(sh, sizeof(sh), "%s%s%s",
                                           procpath(), PATH_SEPARATORSTR, "stop.sh"), sizeof(sh));
        if (has_sh) {
            _stop_sh(sh);
        } else {
            PRINT("stop.sh path too long, skipped.");
        }
        int32_t rtn = service_hug(sync_pipe[1], devnull);
        if (has_sh) {
            remove(sh);//服务退出，移除stop.sh
        }
        return rtn;
    } else if (pid > 0) {
        close(sync_pipe[1]);
        char r = 0;
        ssize_t n;
        //EINTR 重试；read 返 1 + 'R' = service_init 成功；返 0 (EOF) = 子进程失败
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
                    PRINT("child exited with code %d", WEXITSTATUS(wstatus));
                } else if (WIFSIGNALED(wstatus)) {
                    PRINT("child killed by signal %d", WTERMSIG(wstatus));
                }
            }
            return ERR_FAILED;
        }
        return ERR_OK;
    } else {
        PRINT("fork process error!");
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return ERR_FAILED;
    }
#endif
}
