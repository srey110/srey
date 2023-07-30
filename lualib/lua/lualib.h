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

#define LUA_CMSGPACKNAME "cmsgpack"
LUAMOD_API int (luaopen_cmsgpack_safe)(lua_State *L);

#define LUA_PBPACKNAME "pb"
LUAMOD_API int (luaopen_pb)(lua_State *L);

#define LUA_SREYUTILSNAME "srey.utils"
LUAMOD_API int (luaopen_srey_utils)(lua_State *L);

#define LUA_SREYALGO "srey.algo"
LUAMOD_API int (luaopen_srey_algo)(lua_State *L);

#define LUA_SREYNAME "srey.core"
LUAMOD_API int (luaopen_srey)(lua_State *L);

/* open all previous libraries */
LUALIB_API void (luaL_openlibs) (lua_State *L);


#endif
