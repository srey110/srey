#ifndef LPUB_H_
#define LPUB_H_

#include "lib.h"
#if WITH_LUA
#include "lua/lualib.h"
#include "lua/lauxlib.h"

#define CERT_FOLDER "keys"
#define CUR_TASK_NAME "_curtask"
#define PATH_NAME "_propath"
#define PATH_SEP_NAME "_pathsep"
#define MSG_DISP_FUNC "message_dispatch"
#define ASSOC_MTABLE(lua, name) \
    luaL_getmetatable(lua, name);\
    lua_setmetatable(lua, -2)
#define REG_MTABLE(lua, name, regnew, regfunc)\
    luaL_newmetatable(lua, name);\
    lua_pushvalue(lua, -1);\
    lua_setfield(lua, -2, "__index");\
    luaL_setfuncs(lua, regfunc, 0);\
    luaL_newlib(lua, regnew)

void *global_userdata(lua_State *lua, const char *name);
const char *global_string(lua_State *lua, const char *name);

#endif
#endif//LPUB_H_
