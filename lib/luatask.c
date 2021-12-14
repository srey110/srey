#include "luatask.h"
#include "lapi.h"
#include "lstring.h"
#include "lualib.h"
#include "lauxlib.h"
#include "loger.h"

#define LUA_FOLDER "lua"
#ifdef OS_WIN
#define DLL_EXNAME "dll"
#else
#define DLL_EXNAME "so"
#endif
#define LFUNC_INIT "init"
#define LFUNC_RUN  "run"
#define LFUNC_STOP "stop"

static char g_luapath[PATH_LENS] = {0};
void lua_initpath()
{
    ASSERTAB(ERR_OK == getprocpath(g_luapath), "getprocpath failed.");
    SNPRINTF(g_luapath, sizeof(g_luapath) - 1, "%s%s%s%s", 
        g_luapath, PATH_SEPARATORSTR, LUA_FOLDER, PATH_SEPARATORSTR);
}
static inline void _lua_setpath(lua_State *plua, const char *pname, const char *pexname)
{
    lua_getglobal(plua, "package");
    lua_getfield(plua, -1, pname);
    lua_pushfstring(plua, "%s;%s?.%s", lua_tostring(plua, -1), g_luapath, pexname);
    lua_setfield(plua, -3, pname);
    lua_pop(plua, 2);
}
lua_State *lua_newfile(const char *pfile)
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
    char acstartup[PATH_LENS] = { 0 };
    SNPRINTF(acstartup, sizeof(acstartup) - 1, "%s%s", g_luapath, pfile);
    if (LUA_OK != luaL_dofile(plua, acstartup))
    {
        LOG_ERROR("%s", lua_tostring(plua, -1));
        lua_close(plua);
        return NULL;
    }
    return plua;
}
static inline void *lmodule_new(struct task_ctx *ptask, void *pud)
{
    return lua_newfile(pud);
}
static inline int32_t lmodule_init(struct task_ctx *ptask, void *pud)
{
    lua_State *plua = task_handle(ptask);
    lua_getglobal(plua, LFUNC_INIT);
    if (LUA_TFUNCTION == lua_type(plua, 1))
    {
        lua_pushlightuserdata(plua, ptask);
        if (LUA_OK != lua_pcall(plua, 1, 0, 0))
        {
            LOG_ERROR("%s", lua_tostring(plua, 1));
            return ERR_FAILED;
        }
    }
    return ERR_OK;
}
static inline void lmodule_run(struct task_ctx *ptask, uint32_t itype, uint64_t srcid, uint32_t uisess, void *pmsg, uint32_t uisize, void *pud)
{
    lua_State *plua = task_handle(ptask);
    lua_getglobal(plua, LFUNC_RUN);
    ASSERTAB(LUA_TFUNCTION == lua_type(plua, 1), "not have run func.");
    lua_pushlightuserdata(plua, ptask);
    lua_pushinteger(plua, itype);
    lua_pushinteger(plua, srcid);
    lua_pushinteger(plua, uisess);
    switch (itype)
    {
    case MSG_TYPE_REQUEST:
    case MSG_TYPE_RESPONSE:
        lua_pushlstring(plua, pmsg, uisize);
        break;
    default:
        lua_pushlightuserdata(plua, pmsg);
        break;
    }
    lua_pushinteger(plua, uisize);
    if (LUA_OK != lua_pcall(plua, 6, 0, 0))
    {
        LOG_ERROR("%s", lua_tostring(plua, 1));
    }
}
static inline void lmodule_stop(struct task_ctx *ptask, void *pud)
{
    lua_State *plua = task_handle(ptask);
    lua_getglobal(plua, LFUNC_STOP);
    if (LUA_TFUNCTION == lua_type(plua, 1))
    {
        lua_pushlightuserdata(plua, ptask);
        if (LUA_OK != lua_pcall(plua, 1, 0, 0))
        {
            LOG_ERROR("%s", lua_tostring(plua, 1));
        }
    }
}
static inline void lmodule_free(struct task_ctx *ptask, void *pud)
{
    lua_State *plua = task_handle(ptask);
    if (NULL != plua)
    {
        lua_close(plua);
    }
    SAFE_FREE(pud);
}
static int32_t lua_newtask(lua_State *plua)
{
    if (LUA_TSTRING != lua_type(plua, 1)
        || LUA_TSTRING != lua_type(plua, 2)
        || LUA_TNUMBER != lua_type(plua, 3))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
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
    md.md_free = lmodule_free;
    md.md_init = lmodule_init;
    md.md_new = lmodule_new;
    md.md_run = lmodule_run;
    md.md_stop = lmodule_stop;
    memcpy(md.name, pname, ilens);
    md.name[ilens] = '\0';
    ilens = strlen(pfile);
    char *pud = MALLOC(ilens + 1);
    if (NULL == pud)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        lua_pushnil(plua);
        return 1;
    }
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
static int32_t lua_grab(lua_State *plua)
{
    struct task_ctx *ptask = NULL;
    int32_t itype = lua_type(plua, 1);
    if (LUA_TNUMBER == itype)
    {
        ptask = srey_grabid(g_srey, lua_tointeger(plua, 1));
    }
    else if (LUA_TSTRING == itype)
    {
        ptask = srey_grabnam(g_srey, lua_tostring(plua, 1));
    }
    else
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    NULL == ptask ? lua_pushnil(plua) : lua_pushlightuserdata(plua, ptask);
    return 1;
}
static int32_t lua_release(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        return 0;
    }
    srey_release(lua_touserdata(plua, 1));
    return 0;
}
static int32_t lua_task_call(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TSTRING != lua_type(plua, 2))
    {
        LOG_ERROR("%s", "param type error.");
        return 0;
    }
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    size_t ilens;
    const char *pmsg = lua_tolstring(plua, 2, &ilens);
    char *pcall = MALLOC(ilens);
    if (NULL == pcall)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return 0;
    }
    memcpy(pcall, pmsg, ilens);
    srey_call(ptask, pcall, (uint32_t)ilens);
    return 0;
}
static int32_t lua_task_request(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TNUMBER != lua_type(plua, 2)
        || LUA_TNUMBER != lua_type(plua, 3)
        || LUA_TSTRING != lua_type(plua, 4))
    {
        LOG_ERROR("%s", "param type error.");
        return 0;
    }
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    uint64_t srcid = (uint64_t)lua_tointeger(plua, 2);
    uint32_t uisess = (uint32_t)lua_tointeger(plua, 3);
    size_t ilens;
    const char *pmsg = lua_tolstring(plua, 4, &ilens);
    char *pcall = MALLOC(ilens);
    if (NULL == pcall)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return 0;
    }
    memcpy(pcall, pmsg, ilens);
    srey_request(ptask, srcid, uisess, pcall, (uint32_t)ilens);
    return 0;
}
static int32_t lua_task_response(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TNUMBER != lua_type(plua, 2)
        || LUA_TSTRING != lua_type(plua, 3))
    {
        LOG_ERROR("%s", "param type error.");
        return 0;
    }
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    uint32_t uisess = (uint32_t)lua_tointeger(plua, 2);
    size_t ilens;
    const char *pmsg = lua_tolstring(plua, 3, &ilens);
    char *pcall = MALLOC(ilens);
    if (NULL == pcall)
    {
        LOG_ERROR("%s", ERRSTR_MEMORY);
        return 0;
    }
    memcpy(pcall, pmsg, ilens);
    srey_response(ptask, uisess, pcall, (uint32_t)ilens);
    return 0;
}
static int32_t lua_task_id(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    lua_pushinteger(plua, task_id(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t lua_task_name(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 0;
    }
    lua_pushstring(plua, task_name(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t lua_task_newsession(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 0;
    }
    lua_pushinteger(plua, task_new_session(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t lua_timeout(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TNUMBER != lua_type(plua, 2)
        || LUA_TNUMBER != lua_type(plua, 3))
    {
        LOG_ERROR("%s", "param type error.");
        return 0;
    }
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    uint32_t uisess = (uint32_t)lua_tointeger(plua, 2);
    uint32_t uitime = (uint32_t)lua_tointeger(plua, 3);
    srey_timeout(ptask, uisess, uitime);
    return 0;
}
static int32_t lua_listener(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TSTRING != lua_type(plua, 2)
        || LUA_TNUMBER != lua_type(plua, 3))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    const char *phost = lua_tostring(plua, 2);
    uint16_t usport = (uint16_t)lua_tointeger(plua, 3);
    struct listener_ctx *plsn = srey_listener(ptask, phost, usport);
    NULL == plsn ? lua_pushnil(plua) : lua_pushlightuserdata(plua, plsn);
    return 1;
}
static int32_t lua_freelsn(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        return 0;
    }
    srey_freelsn(lua_touserdata(plua, 1));
    return 0;
}
static int32_t lua_connecter(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TNUMBER != lua_type(plua, 2)
        || LUA_TNUMBER != lua_type(plua, 3)
        || LUA_TSTRING != lua_type(plua, 4)
        || LUA_TNUMBER != lua_type(plua, 5))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    uint32_t uisess = (uint32_t)lua_tointeger(plua, 2);
    uint32_t utimeout = (uint32_t)lua_tointeger(plua, 3);
    const char *phost = lua_tostring(plua, 4);
    uint16_t usport = (uint16_t)lua_tointeger(plua, 5);
    struct sock_ctx *psock = srey_connecter(ptask, uisess, utimeout, phost, usport);
    NULL == psock ? lua_pushnil(plua) : lua_pushlightuserdata(plua, psock);
    return 1;
}
static int32_t lua_newsock(lua_State *plua)
{
    if (LUA_TNUMBER != lua_type(plua, 1)
        || LUA_TNUMBER != lua_type(plua, 2)
        || LUA_TNUMBER != lua_type(plua, 3))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    SOCKET sock = (SOCKET)lua_tointeger(plua, 1);
    int32_t itype = (int32_t)lua_tointeger(plua, 2);
    int32_t ifamily = (int32_t)lua_tointeger(plua, 3);
    struct sock_ctx *psock = srey_newsock(g_srey, sock, itype, ifamily);
    NULL == psock ? lua_pushnil(plua) : lua_pushlightuserdata(plua, psock);
    return 1;
}
static int32_t lua_enable(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TLIGHTUSERDATA != lua_type(plua, 2)
        || LUA_TBOOLEAN != lua_type(plua, 3))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushboolean(plua, 0);
        return 1;
    }
    struct task_ctx *ptask = lua_touserdata(plua, 1);
    struct sock_ctx *psock = lua_touserdata(plua, 2);
    int32_t iwrite = (int32_t)lua_tointeger(plua, 3);
    int32_t irtn = srey_enable(ptask, psock, iwrite);
    ERR_OK == irtn ? lua_pushboolean(plua, 1) : lua_pushboolean(plua, 0);
    return 1;
}
static int32_t lua_sock_id(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    lua_pushinteger(plua, sock_id(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t lua_sock_handle(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    lua_pushinteger(plua, sock_handle(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t lua_sock_type(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    lua_pushinteger(plua, sock_type(lua_touserdata(plua, 1)));
    return 1;
}
static int32_t lua_sock_send(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushboolean(plua, 0);
        return 1;
    }
    int32_t irtn;
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    if (SOCK_STREAM == sock_type(psock))
    {
        if (LUA_TSTRING != lua_type(plua, 2))
        {
            LOG_ERROR("%s", "param type error.");
            lua_pushboolean(plua, 0);
            return 1;
        }
        size_t ilens;
        const char *pmsg = lua_tolstring(plua, 2, &ilens);
        irtn = sock_send(psock, (void *)pmsg, ilens);
    }
    else
    {
        if (LUA_TSTRING != lua_type(plua, 2)
            || LUA_TSTRING != lua_type(plua, 3)
            || LUA_TNUMBER != lua_type(plua, 4))
        {
            LOG_ERROR("%s", "param type error.");
            lua_pushboolean(plua, 0);
            return 1;
        }
        size_t ilens;
        const char *pmsg = lua_tolstring(plua, 2, &ilens);
        const char *phost = lua_tostring(plua, 3);
        uint16_t usport = (uint16_t)lua_tointeger(plua, 4);
        irtn = sock_sendto(psock, phost, usport, (void *)pmsg, ilens);
    }
    ERR_OK == irtn ? lua_pushboolean(plua, 1) : lua_pushboolean(plua, 0);
    return 1;
}
static int32_t lua_sock_close(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        return 0;
    }
    sock_close(lua_touserdata(plua, 1));
    return 0;
}
static int32_t lua_udp_msg(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    struct udp_recv_msg *pudp = lua_touserdata(plua, 1);
    lua_pushstring(plua, pudp->ip);
    lua_pushinteger(plua, pudp->port);
    lua_pushlightuserdata(plua, pudp->sock);
    return 3;
}
static int32_t lua_buf_size(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushinteger(plua, 0);
        return 1;
    }
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    lua_pushinteger(plua, buffer_size(sock_buffer_r(psock)));
    return 1;
}
static int32_t lua_buf_copy(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TNUMBER != lua_type(plua, 2))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    size_t ilens = (size_t)lua_tointeger(plua, 2);
    struct buffer_ctx *pbuf = sock_buffer_r(psock);
    if (0 == ilens
        || 0 == buffer_size(pbuf))
    {
        lua_pushnil(plua);
        return 1;
    }
    TString *ts = luaS_createlngstrobj(plua, ilens);
    int32_t irtn = buffer_copyout(pbuf, getstr(ts), ilens);
    if (irtn <= 0)
    {
        lua_pushnil(plua);
        return 1;
    }
    ts->u.lnglen = (size_t)irtn;
    setsvalue2s(plua, plua->top, ts);
    api_incr_top(plua);
    luaC_checkGC(plua);
    return 1;
}
static int32_t lua_buf_drain(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TNUMBER != lua_type(plua, 2))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushinteger(plua, 0);
        return 1;
    }
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    size_t ilens = (size_t)lua_tointeger(plua, 2);
    int32_t irtn = buffer_drain(sock_buffer_r(psock), ilens);
    lua_pushinteger(plua, irtn);
    return 1;
}
static int32_t lua_buf_remove(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TNUMBER != lua_type(plua, 2))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushnil(plua);
        return 1;
    }
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    size_t ilens = (size_t)lua_tointeger(plua, 2);
    struct buffer_ctx *pbuf = sock_buffer_r(psock);
    if (0 == ilens
        || 0 == buffer_size(pbuf))
    {
        lua_pushnil(plua);
        return 1;
    }
    TString *ts = luaS_createlngstrobj(plua, ilens);
    int32_t irtn = buffer_remove(pbuf, getstr(ts), ilens);
    if (irtn <= 0)
    {
        lua_pushnil(plua);
        return 1;
    }
    ts->u.lnglen = (size_t)irtn;
    setsvalue2s(plua, plua->top, ts);
    api_incr_top(plua);
    luaC_checkGC(plua);
    return 1;
}
static int32_t lua_buf_search(lua_State *plua)
{
    if (LUA_TLIGHTUSERDATA != lua_type(plua, 1)
        || LUA_TNUMBER != lua_type(plua, 2)
        || LUA_TSTRING != lua_type(plua, 3))
    {
        LOG_ERROR("%s", "param type error.");
        lua_pushinteger(plua, -1);
        return 1;
    }
    struct sock_ctx *psock = lua_touserdata(plua, 1);
    size_t uistart = (size_t)lua_tointeger(plua, 2);
    size_t ilens;
    const char *pwhat = lua_tolstring(plua, 3, &ilens);
    int32_t irtn = buffer_search(sock_buffer_r(psock), uistart, (void *)pwhat, ilens);
    lua_pushinteger(plua, irtn);
    return 1;
}
LUAMOD_API int luaopen_srey(lua_State *L)
{
    luaL_Reg reg[] =
    {
        { "newtask", lua_newtask },//newtask("xxx.lua", "taskname", max run count) return  nil/task
        { "grab", lua_grab },//grab(id/taskname) return  nil/task
        { "release", lua_release },//release(task)
        { "call", lua_task_call },//call(task, msg)
        { "request", lua_task_request },//request(task, srcid, session, msg)
        { "response", lua_task_response },//response(task, session, msg)
        { "taskid", lua_task_id },//taskid(task) nil/id
        { "taskname", lua_task_name },//taskname(task) nil/name
        { "newsession", lua_task_newsession },//newsession(task) nil/session
        { "timeout", lua_timeout },//timeout(task, session, timeout)
        { "listener", lua_listener },//listener(task, ip , port) return  nil/listener
        { "freelsn", lua_freelsn },//freelsn(listener)
        { "connecter", lua_connecter },//connecter(task, session, timeout, ip , port)  nil/sock
        { "addsock", lua_newsock },//addsock(fd, type, family)  nil/sock
        { "enablerw", lua_enable },//enablerw(task, sock, bwrite) bool
        { "sockid", lua_sock_id },//sockid(sock) nil/id
        { "sock", lua_sock_handle },//sock(sock) nil/fd
        { "socktype", lua_sock_type },//socktype(sock) nil/type
        { "socksend", lua_sock_send },//tcp:socksend(sock, msg) udp:socksend(sock, msg, ip , port) bool 
        { "sockclose", lua_sock_close },//sockclose(sock)
        { "udpmsg", lua_udp_msg },//udpmsg(msg)  ip port sock
        { "bufsize", lua_buf_size },//bufsize(sock)
        { "bufcopy", lua_buf_copy },//bufcopy(sock, lens)  nil/lstring
        { "bufdrain", lua_buf_drain },//bufdrain(sock, lens)  -1/lens
        { "bufremove", lua_buf_remove },//bufremove(sock, lens) nil/lstring
        { "bufsearch", lua_buf_search },//bufsearch(sock, start, what)  -1/pos
        { NULL, NULL },
    };
    luaL_newlib(L, reg);
    return 1;
}
