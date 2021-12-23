#include "luatask.h"
#include "lapi.h"
#include "lstring.h"
#include "lualib.h"
#include "lauxlib.h"
#include "loger.h"
#include "utils.h"

#ifdef OS_WIN
#define DLL_EXNAME "dll"
#else
#define DLL_EXNAME "so"
#endif

static char g_luapath[PATH_LENS] = {0};
void initpath()
{
    char acpath[PATH_LENS];
    ASSERTAB(ERR_OK == getprocpath(acpath), "getprocpath failed.");
    SNPRINTF(g_luapath, sizeof(g_luapath) - 1, "%s%s%s%s", 
        acpath, PATH_SEPARATORSTR, "lua", PATH_SEPARATORSTR);
}
static inline void _lua_setpath(lua_State *plua, const char *pname, const char *pexname)
{
    lua_getglobal(plua, "package");
    lua_getfield(plua, -1, pname);
    lua_pushfstring(plua, "%s;%s?.%s", lua_tostring(plua, -1), g_luapath, pexname);
    lua_setfield(plua, -3, pname);
    lua_pop(plua, 2);
}
static inline lua_State *_lua_init(void *ptask)
{
    lua_State *plua = luaL_newstate();
    if (NULL == plua)
    {
        LOG_ERROR("%s", "luaL_newstate failed.");
        return NULL;
    }

    luaL_openlibs(plua);
    _lua_setpath(plua, "cpath", DLL_EXNAME);
    _lua_setpath(plua, "path", "lua");
    if (NULL != ptask)
    {
        lua_pushlightuserdata(plua, ptask);
        lua_setglobal(plua, "_task");
    }
    return plua;
}
static inline int32_t _lua_dofile(lua_State *plua, const char *pfile)
{
    char acfile[PATH_LENS] = { 0 };
    SNPRINTF(acfile, sizeof(acfile) - 1, "%s%s", g_luapath, pfile);
    if (LUA_OK != luaL_dofile(plua, acfile))
    {
        LOG_ERROR("%s", lua_tostring(plua, -1));
        return ERR_FAILED;
    }
    return ERR_OK;
}
int32_t lua_startup()
{
    lua_State *plua = _lua_init(NULL);
    if (NULL == plua)
    {
        return ERR_FAILED;
    }
    int32_t irtn = _lua_dofile(plua, "startup.lua");
    lua_close(plua);
    return irtn;
}
static inline void *_lmodule_new(struct task_ctx *ptask, void *pud)
{
    return _lua_init(ptask);
}
static inline int32_t _lmodule_init(struct task_ctx *ptask, void *handle, void *pud)
{
    return _lua_dofile(handle, pud);
}
static inline void _lmodule_run(struct task_ctx *ptask, void *handle, uint32_t itype, 
    uint64_t srcid, uint64_t uisess, void *pmsg, uint32_t uisize, void *pud)
{
    lua_State *plua = (lua_State *)handle;
    lua_getglobal(plua, "_dispatch_message");
    ASSERTAB(LUA_TFUNCTION == lua_type(plua, 1), "not have _dispatch_message.");
    lua_pushinteger(plua, itype);
    lua_pushinteger(plua, srcid);
    lua_pushinteger(plua, uisess);
    lua_pushlightuserdata(plua, pmsg);
    lua_pushinteger(plua, uisize);
    if (LUA_OK != lua_pcall(plua, 5, 0, 0))
    {
        LOG_ERROR("%s", lua_tostring(plua, 1));
    }
}
static inline void _lmodule_free(struct task_ctx *ptask, void *handle, void *pud)
{
    lua_State *plua = (lua_State *)handle;
    if (NULL != plua)
    {
        lua_close(plua);
    }
    SAFE_FREE(pud);
}
static int32_t _lua_log(lua_State *plua)
{
    LOG_LEVEL emlv = (LOG_LEVEL)lua_tointeger(plua, 1);
    const char *pfile = lua_tostring(plua, 2);
    int32_t iline = (int32_t)lua_tointeger(plua, 3);
    const char *plog = lua_tostring(plua, 4);
    loger_log(&g_logerctx, emlv, "[%s][%s %d]%s", _getlvstr(emlv), __FILENAME__(pfile), iline, plog);
    return 0;
}
static int32_t _lua_newtask(lua_State *plua)
{
    const char *pfile = lua_tostring(plua, 1);
    const char *pname = lua_tostring(plua, 2);
    size_t ilens = strlen(pname);
    if (ilens >= NAME_LENS)
    {
        LOG_ERROR("task name %s lens error.", pname);
        lua_pushnil(plua);
        return 1;
    }
    int32_t icnt = (int32_t)lua_tointeger(plua, 3);
    icnt = (icnt <= 0 ? 1 : icnt);
    struct module_ctx md;
    md.maxcnt = icnt;
    md.md_free = _lmodule_free;
    md.md_init = _lmodule_init;
    md.md_new = _lmodule_new;
    md.md_run = _lmodule_run;
    memcpy(md.name, pname, ilens);
    md.name[ilens] = '\0';
    ilens = strlen(pfile);
    char *pud = MALLOC(ilens + 1);
    ASSERTAB(NULL != pud, ERRSTR_MEMORY);
    memcpy(pud, pfile, ilens);
    pud[ilens] = '\0';
    struct task_ctx *ptask = srey_newtask(g_srey, &md, pud);
    if (NULL == ptask)
    {
        lua_pushnil(plua);
        FREE(pud);
    }
    else
    {
        lua_pushlightuserdata(plua, ptask);
    }
    return 1;
}
static int32_t _lua_grab(lua_State *plua)
{
    struct task_ctx *ptask = NULL;
    if (LUA_TNUMBER == lua_type(plua, 1))
    {
        ptask = srey_grabid(g_srey, lua_tointeger(plua, 1));
    }
    else
    {
        ptask = srey_grabnam(g_srey, lua_tostring(plua, 1));
    }
    NULL == ptask ? lua_pushnil(plua) : lua_pushlightuserdata(plua, ptask);
    return 1;
}
static int32_t _lua_release(lua_State *plua)
{
    srey_release(lua_touserdata(plua, 1));
    return 0;
}
static int32_t _lua_task_call(lua_State *plua)
{
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    size_t ilens;
    const char *pmsg = lua_tolstring(plua, 2, &ilens);
    char *pcall = MALLOC(ilens + 1);
    ASSERTAB(NULL != pcall, ERRSTR_MEMORY);
    memcpy(pcall, pmsg, ilens);
    pcall[ilens] = '\0';
    srey_call(ptask, pcall, (uint32_t)ilens);
    return 0;
}
static int32_t _lua_task_request(lua_State *plua)
{
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    uint64_t srcid = (uint64_t)lua_tointeger(plua, 2);
    uint64_t uisess = (uint64_t)lua_tointeger(plua, 3);
    size_t ilens;
    const char *pmsg = lua_tolstring(plua, 4, &ilens);
    char *pcall = MALLOC(ilens + 1);
    ASSERTAB(NULL != pcall, ERRSTR_MEMORY);
    memcpy(pcall, pmsg, ilens);
    pcall[ilens] = '\0';
    srey_request(ptask, srcid, uisess, pcall, (uint32_t)ilens);
    return 0;
}
static int32_t _lua_task_response(lua_State *plua)
{
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    uint64_t uisess = (uint64_t)lua_tointeger(plua, 2);
    size_t ilens;
    const char *pmsg = lua_tolstring(plua, 3, &ilens);
    char *pcall = MALLOC(ilens + 1);
    ASSERTAB(NULL != pcall, ERRSTR_MEMORY);
    memcpy(pcall, pmsg, ilens);
    pcall[ilens] = '\0';
    srey_response(ptask, uisess, pcall, (uint32_t)ilens);
    return 0;
}
static int32_t _lua_task_id(lua_State *plua)
{
    lua_pushinteger(plua, task_id(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t _lua_task_name(lua_State *plua)
{
    lua_pushstring(plua, task_name(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t _lua_timeout(lua_State *plua)
{
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    uint64_t uisess = (uint64_t)lua_tointeger(plua, 2);
    uint32_t uitime = (uint32_t)lua_tointeger(plua, 3);
    srey_timeout(ptask, uisess, uitime);
    return 0;
}
static int32_t _lua_listener(lua_State *plua)
{
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    uint64_t uisess = (uint64_t)lua_tointeger(plua, 2);
    const char *phost = lua_tostring(plua, 3);
    uint16_t usport = (uint16_t)lua_tointeger(plua, 4);
    struct listener_ctx *plsn = srey_listener(ptask, uisess, phost, usport);
    NULL == plsn ? lua_pushnil(plua) : lua_pushlightuserdata(plua, plsn);
    return 1;
}
static int32_t _lua_listener_sess(lua_State *plua)
{
    lua_pushinteger(plua, listener_ud(lua_touserdata(plua, 1))->session);
    return 1;
}
static int32_t _lua_freelsn(lua_State *plua)
{
    srey_freelsn(lua_touserdata(plua, 1));
    return 0;
}
static int32_t _lua_connecter(lua_State *plua)
{
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    uint64_t uisess = (uint64_t)lua_tointeger(plua, 2);
    uint32_t utimeout = (uint32_t)lua_tointeger(plua, 3);
    const char *phost = lua_tostring(plua, 4);
    uint16_t usport = (uint16_t)lua_tointeger(plua, 5);
    struct sock_ctx *psock = srey_connecter(ptask, uisess, utimeout, phost, usport);
    NULL == psock ? lua_pushnil(plua) : lua_pushlightuserdata(plua, psock);
    return 1;
}
static int32_t _lua_newsock(lua_State *plua)
{
    SOCKET sock = (SOCKET)lua_tointeger(plua, 1);
    int32_t itype = (int32_t)lua_tointeger(plua, 2);
    int32_t ifamily = (int32_t)lua_tointeger(plua, 3);
    struct sock_ctx *psock = srey_newsock(g_srey, sock, itype, ifamily);
    NULL == psock ? lua_pushnil(plua) : lua_pushlightuserdata(plua, psock);
    return 1;
}
static int32_t _lua_enable(lua_State *plua)
{
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    struct sock_ctx *psock = lua_touserdata(plua, 2);
    uint64_t uisess = (uint64_t)lua_tointeger(plua, 3);
    int32_t iwrite = (int32_t)lua_tointeger(plua, 4);
    int32_t irtn = srey_enable(ptask, psock, uisess, iwrite);
    ERR_OK == irtn ? lua_pushboolean(plua, 1) : lua_pushboolean(plua, 0);
    return 1;
}
static int32_t _lua_sock_id(lua_State *plua)
{
    lua_pushinteger(plua, sock_id(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t _lua_sock_handle(lua_State *plua)
{
    lua_pushinteger(plua, sock_handle(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t _lua_sock_type(lua_State *plua)
{
    lua_pushinteger(plua, sock_type(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t _lua_sock_sess(lua_State *plua)
{
    lua_pushinteger(plua, sock_ud(lua_touserdata(plua, 1))->session);
    return 1;
}
static int32_t _lua_sock_send(lua_State *plua)
{
    int32_t irtn;
    size_t ilens;
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    const char *pmsg = lua_tolstring(plua, 2, &ilens);
    if (SOCK_STREAM == sock_type(psock))
    {
        irtn = sock_send(psock, (void *)pmsg, ilens);
    }
    else
    {
        const char *phost = lua_tostring(plua, 3);
        uint16_t usport = (uint16_t)lua_tointeger(plua, 4);
        irtn = sock_sendto(psock, phost, usport, (void *)pmsg, ilens);
    }
    ERR_OK == irtn ? lua_pushboolean(plua, 1) : lua_pushboolean(plua, 0);
    return 1;
}
static int32_t _lua_sock_close(lua_State *plua)
{
    sock_close(lua_touserdata(plua, 1));
    return 0;
}
static int32_t _lua_udp_msg(lua_State *plua)
{
    struct udp_recv_msg *pudp = lua_touserdata(plua, 1);
    lua_pushstring(plua, pudp->ip);
    lua_pushinteger(plua, pudp->port);
    lua_pushlightuserdata(plua, pudp->sock);
    return 3;
}
static int32_t _lua_buf_size(lua_State *plua)
{
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    lua_pushinteger(plua, buffer_size(sock_buffer_r(psock)));
    return 1;
}
static inline void _lua_push_buffer(lua_State *plua, 
    struct buffer_ctx *pbuf, size_t uilens, int32_t idel)
{
    buffer_lock(pbuf);
    if (uilens > pbuf->total_len)
    {
        uilens = pbuf->total_len;
    }
    if (0 == uilens
        || 0 != pbuf->freeze_read)
    {
        lua_pushnil(plua);
        buffer_unlock(pbuf);
        return;
    }

    TString *pts = luaS_createlngstrobj(plua, uilens);
    int32_t irtn = _buffer_copyout(pbuf, getstr(pts), uilens);
    ASSERTAB(irtn == uilens, "copyout lens not equ request lens.");
    if (0 != idel)
    {
        ASSERTAB(uilens == _buffer_drain(pbuf, uilens), "drain lens not equ request lens.");
    }
    buffer_unlock(pbuf);

    setsvalue2s(plua, plua->top, pts);
    api_incr_top(plua);
    luaC_checkGC(plua);
}
static int32_t _lua_buf_copy(lua_State *plua)
{
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    size_t ilens = (size_t)lua_tointeger(plua, 2);
    struct buffer_ctx *pbuf = sock_buffer_r(psock);
    _lua_push_buffer(plua, pbuf, ilens, 0);
    return 1;
}
static int32_t _lua_buf_drain(lua_State *plua)
{
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    size_t ilens = (size_t)lua_tointeger(plua, 2);
    int32_t irtn = buffer_drain(sock_buffer_r(psock), ilens);
    lua_pushinteger(plua, irtn);
    return 1;
}
static int32_t _lua_buf_remove(lua_State *plua)
{
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    size_t ilens = (size_t)lua_tointeger(plua, 2);
    struct buffer_ctx *pbuf = sock_buffer_r(psock);
    _lua_push_buffer(plua, pbuf, ilens, 1);
    return 1;
}
static int32_t _lua_buf_search(lua_State *plua)
{
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    size_t uistart = (size_t)lua_tointeger(plua, 2);
    size_t ilens;
    const char *pwhat = lua_tolstring(plua, 3, &ilens);
    int32_t irtn = buffer_search(sock_buffer_r(psock), uistart, (void *)pwhat, ilens);
    lua_pushinteger(plua, irtn);
    return 1;
}
static int32_t _lua_millisecond(lua_State *plua)
{
    lua_pushinteger(plua, nowmsec());
    return 1;
}
LUAMOD_API int luaopen_srey(lua_State *plua)
{
    luaL_Reg reg[] =
    {
        { "log", _lua_log },
        { "newtask", _lua_newtask },//newtask("xxx.lua", "taskname", max run count) return  nil/task
        { "grab", _lua_grab },//grab(id/taskname) return  nil/task
        { "release", _lua_release },//release(task)
        { "call", _lua_task_call },//call(task, msg)
        { "request", _lua_task_request },//request(task, srcid, session, msg)
        { "response", _lua_task_response },//response(task, session, msg)
        { "taskid", _lua_task_id },//taskid(task) nil/id
        { "taskname", _lua_task_name },//taskname(task) nil/name
        { "timeout", _lua_timeout },//timeout(task, session, timeout)
        { "listener", _lua_listener },//listener(task, session, ip , port) return  nil/listener
        { "listenersess", _lua_listener_sess },
        { "freelsn", _lua_freelsn },//freelsn(listener)
        { "connecter", _lua_connecter },//connecter(task, session, timeout, ip , port)  nil/sock
        { "addsock", _lua_newsock },//addsock(fd, type, family)  nil/sock
        { "enablerw", _lua_enable },//enablerw(task, sock, session, bwrite) bool
        { "sockid", _lua_sock_id },//sockid(sock) nil/id
        { "socksess", _lua_sock_sess },
        { "sock", _lua_sock_handle },//sock(sock) nil/fd
        { "socktype", _lua_sock_type },//socktype(sock) nil/type
        { "socksend", _lua_sock_send },//tcp:socksend(sock, msg) udp:socksend(sock, msg, ip , port) bool 
        { "sockclose", _lua_sock_close },//sockclose(sock)
        { "udpmsg", _lua_udp_msg },//udpmsg(msg)  ip port sock
        { "bufsize", _lua_buf_size },//bufsize(sock)
        { "bufcopy", _lua_buf_copy },//bufcopy(sock, lens)  nil/lstring
        { "bufdrain", _lua_buf_drain },//bufdrain(sock, lens)  -1/lens
        { "bufremove", _lua_buf_remove },//bufremove(sock, lens) nil/lstring
        { "bufsearch", _lua_buf_search },//bufsearch(sock, start, what)  -1/pos

        { "millisecond", _lua_millisecond },
        { NULL, NULL },
    };
    luaL_newlib(plua, reg);
    return 1;
}
