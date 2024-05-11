/*
** $Id: lualib.h $
** Lua standard libraries
** See Copyright Notice in lua.h
*/


#ifndef lualib_h
#define lualib_h

#include "lua.h"


/* version suffix for environment variable names */
#define LUA_VERSUFFIX          "_" LUA_VERSION_MAJOR "_" LUA_VERSION_MINOR


LUAMOD_API int (luaopen_base) (lua_State *L);

#define LUA_COLIBNAME	"coroutine"
LUAMOD_API int (luaopen_coroutine) (lua_State *L);

#define LUA_TABLIBNAME	"table"
LUAMOD_API int (luaopen_table) (lua_State *L);

#define LUA_IOLIBNAME	"io"
LUAMOD_API int (luaopen_io) (lua_State *L);

#define LUA_OSLIBNAME	"os"
LUAMOD_API int (luaopen_os) (lua_State *L);

#define LUA_STRLIBNAME	"string"
LUAMOD_API int (luaopen_string) (lua_State *L);

#define LUA_UTF8LIBNAME	"utf8"
LUAMOD_API int (luaopen_utf8) (lua_State *L);

#define LUA_MATHLIBNAME	"math"
LUAMOD_API int (luaopen_math) (lua_State *L);

#define LUA_DBLIBNAME	"debug"
LUAMOD_API int (luaopen_debug) (lua_State *L);

#define LUA_LOADLIBNAME	"package"
LUAMOD_API int (luaopen_package) (lua_State *L);

#define LUA_CJSONLIBNAME "cjson"
LUAMOD_API int (luaopen_cjson)(lua_State *L);

#define LUA_PBPACKNAME "pb"
LUAMOD_API int (luaopen_pb)(lua_State *L);

#define LUA_SREYTASK "srey.task"
LUAMOD_API int (luaopen_task)(lua_State *L);

#define LUA_SREYCORE "srey.core"
LUAMOD_API int (luaopen_core)(lua_State *L);

#define LUA_SREYHARBOR "srey.harbor"
LUAMOD_API int (luaopen_harbor)(lua_State *L);

#define LUA_SREYDNS "srey.dns"
LUAMOD_API int (luaopen_dns)(lua_State *L);

#define LUA_SREYSIMPLE "srey.simple"
LUAMOD_API int (luaopen_simple)(lua_State *L);

#define LUA_SREYWEBSOCK "srey.websock"
LUAMOD_API int (luaopen_websock)(lua_State *L);

#define LUA_SREYHTTP "srey.http"
LUAMOD_API int (luaopen_http)(lua_State *L);

/* open all previous libraries */
LUALIB_API void (luaL_openlibs) (lua_State *L);


#endif
